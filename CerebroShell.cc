#include <cstdio>
#include <mutex>
#include <thread>
#include <atomic>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <pty.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>

#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <cmath>

static std::atomic<bool> input_blocked(false); // when true, char input is ignored (used during AI confirm)

// ---------- Config ----------
const int WINDOW_W = 1000;
const int WINDOW_H = 600;

int CHAR_W = 10;
int CHAR_H = 18;
int COLS = 80;
int ROWS = 24;

const int FIRST_CHAR = 32;
const int LAST_CHAR  = 126;

// ---------- Glyph info ----------
struct GlyphInfo {
    float ax, ay; // advance
    float bw, bh; // bitmap size
    float bl, bt; // bitmap left/top
    float tx, ty; // atlas uv
    float tw, th; // atlas uv size
};

// ---------- Color ----------
struct Color { float r,g,b; };
Color ansi_basic_color(int idx){
    static const Color map[8] = {
        {0,0,0},
        {0.78f,0,0},
        {0,0.78f,0},
        {0.78f,0.78f,0},
        {0,0,0.78f},
        {0.78f,0,0.78f},
        {0,0.78f,0.78f},
        {0.85f,0.85f,0.85f}
    };
    if(idx<0) idx=0; if(idx>7) idx=7; return map[idx];
}

// ---------- Globals ----------
static int master_fd = -1;
static std::vector<std::string> termBuf;
static std::vector<std::vector<Color>> termColor;
static int cursor_x = 0, cursor_y = 0;
static Color cur_fg = {1,1,1};
static Color cur_bg = {0,0,0};

// FreeType & GL atlas
static int ATLAS_W = 2048, ATLAS_H = 2048;
static GLuint atlasTex = 0;
static std::vector<GlyphInfo> glyphs; // indexed by c - FIRST_CHAR

// GL objects
static GLuint programID = 0;
static GLuint vao = 0, vbo = 0;
static GLint uniRes = -1;
static GLint uniTex  = -1;

// --- AI / input buffer Globals ---
static std::string shell_buffer;       // authoritative buffer of what will be sent to shell on Enter
static std::string ai_result = "";     // raw AI text when ready
static bool ai_ready = false;
static std::mutex ai_mutex;
static std::string pending_ai_cmd = "";
static bool awaiting_confirm = false;

// ------- Shaders -------
static const char *vertex_shader_src =
"#version 330 core\n"
"layout(location=0) in vec2 in_pos;\n"
"layout(location=1) in vec2 in_uv;\n"
"layout(location=2) in vec3 in_col;\n"
"out vec2 uv; out vec3 col;\n"
"uniform vec2 u_resolution;\n"
"void main(){ vec2 pos = in_pos / u_resolution * 2.0 - 1.0; pos.y *= -1.0; gl_Position = vec4(pos,0,1); uv = in_uv; col = in_col; }\n";

static const char *fragment_shader_src =
"#version 330 core\n"
"in vec2 uv; in vec3 col; out vec4 out_color;\n"
"uniform sampler2D u_tex;\n"
"void main(){ float a = texture(u_tex, uv).r; out_color = vec4(col, a); }\n";

// ---------- GL helper ----------
static GLuint compile_shader(GLenum type, const char* src){
    GLuint s = glCreateShader(type);
    glShaderSource(s,1,&src,nullptr);
    glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[1024]; glGetShaderInfoLog(s,1024,nullptr,log); std::cerr<<"Shader error: "<<log<<"\n"; std::exit(1); }
    return s;
}
static GLuint build_program(){
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    GLuint p = glCreateProgram();
    glAttachShader(p,vs); glAttachShader(p,fs);
    glLinkProgram(p);
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[1024]; glGetProgramInfoLog(p,1024,nullptr,log); std::cerr<<"Link error: "<<log<<"\n"; std::exit(1); }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// ---------- FreeType atlas builder ----------
static void build_glyph_atlas(const char* fontfile, int pixel_size){
    FT_Library ft;
    if(FT_Init_FreeType(&ft)){ std::cerr<<"FT init failed\n"; std::exit(1); }
    FT_Face face;
    if(FT_New_Face(ft,fontfile,0,&face)){ std::cerr<<"Failed to load font "<<fontfile<<"\n"; std::exit(1); }
    FT_Set_Pixel_Sizes(face,0,pixel_size);

    // measure representative glyph for CHAR_W/CHAR_H
    if(FT_Load_Char(face,'M',FT_LOAD_RENDER)==0){
        CHAR_W = std::max(CHAR_W, (int)face->glyph->bitmap.width + 2);
        CHAR_H = std::max(CHAR_H, (int)face->glyph->bitmap.rows + 2);
    }

    std::vector<unsigned char> atlas(ATLAS_W * ATLAS_H, 0);
    int pen_x = 2, pen_y = 2, row_h = 0;
    glyphs.resize(LAST_CHAR - FIRST_CHAR + 1);

    for(int c = FIRST_CHAR; c <= LAST_CHAR; ++c){
        if(FT_Load_Char(face, c, FT_LOAD_RENDER)){
            // fallback: empty glyph
            GlyphInfo gi{}; glyphs[c - FIRST_CHAR] = gi;
            continue;
        }
        FT_GlyphSlot g = face->glyph;
        int gw = g->bitmap.width;
        int gh = g->bitmap.rows;
        if(pen_x + gw + 2 >= ATLAS_W){
            pen_x = 2;
            pen_y += row_h + 2;
            row_h = 0;
        }
        if(pen_y + gh + 2 >= ATLAS_H){ std::cerr<<"Atlas too small\n"; std::exit(1); }
        // copy
        for(int r=0;r<gh;++r){
            memcpy(&atlas[(pen_y + r)*ATLAS_W + pen_x], &g->bitmap.buffer[r*gw], gw);
        }
        GlyphInfo gi;
        gi.ax = (float)g->advance.x / 64.0f;
        gi.ay = (float)g->advance.y / 64.0f;
        gi.bw = gw; gi.bh = gh;
        gi.bl = g->bitmap_left; gi.bt = g->bitmap_top;
        gi.tx = (float)pen_x / ATLAS_W; gi.ty = (float)pen_y / ATLAS_H;
        gi.tw = (float)gw / ATLAS_W; gi.th = (float)gh / ATLAS_H;
        glyphs[c - FIRST_CHAR] = gi;
        pen_x += gw + 2;
        if(gh > row_h) row_h = gh;
    }

    glGenTextures(1, &atlasTex);
    glBindTexture(GL_TEXTURE_2D, atlasTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, ATLAS_W, ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    FT_Done_Face(face);
    FT_Done_FreeType(ft);
}

// ---------- Terminal buffer helpers ----------
static void clear_screen(){
    for(int r=0;r<ROWS;++r){
        termBuf[r].assign(COLS, ' ');
        termColor[r].assign(COLS, Color{1,1,1});
    }
    cursor_x = cursor_y = 0;
}
static void clear_line_from(int row,int col){
    if(row<0 || row>=ROWS) return;
    for(int c=col;c<COLS;++c){ termBuf[row][c] = ' '; termColor[row][c] = cur_fg; }
}
// Put a single char into the current cursor position (visual only) and advance cursor.
static void put_char_local(char ch){
    if(ch=='\r') return;
    if(ch=='\n'){
        cursor_x = 0; cursor_y++;
        if(cursor_y >= ROWS){
            termBuf.erase(termBuf.begin());
            termColor.erase(termColor.begin());
            termBuf.emplace_back(std::string(COLS,' '));
            termColor.emplace_back(std::vector<Color>(COLS, Color{1,1,1}));
            cursor_y = ROWS-1;
        }
        return;
    }
    if(ch == '\t'){
        int to = (cursor_x / 8 + 1) * 8;
        while(cursor_x < to && cursor_x < COLS){
            termBuf[cursor_y][cursor_x] = ' ';
            termColor[cursor_y][cursor_x] = cur_fg;
            cursor_x++;
        }
        return;
    }
    if(ch == 0x7f || ch == '\b'){
        if(cursor_x>0){ cursor_x--; termBuf[cursor_y][cursor_x] = ' '; termColor[cursor_y][cursor_x] = cur_fg; }
        return;
    }
    unsigned char uc = (unsigned char)ch;
    if(uc < FIRST_CHAR || uc > LAST_CHAR) uc = '?';
    termBuf[cursor_y][cursor_x] = (char)uc;
    termColor[cursor_y][cursor_x] = cur_fg;
    cursor_x++;
    if(cursor_x >= COLS){
        cursor_x = 0; cursor_y++;
        if(cursor_y >= ROWS){
            termBuf.erase(termBuf.begin());
            termColor.erase(termColor.begin());
            termBuf.emplace_back(std::string(COLS,' '));
            termColor.emplace_back(std::vector<Color>(COLS, Color{1,1,1}));
            cursor_y = ROWS-1;
        }
    }
}

// ---------- ANSI parser (for input from shell) ----------
enum ParseState { PS_NORMAL, PS_ESC, PS_CSI, PS_OSC };
static ParseState pstate = PS_NORMAL;
static std::string esc_buf;
static std::string osc_buf;

static void handle_csi_sequence(const std::string &seq){
    if(seq.empty()) return;
    char final_byte = seq.back();
    std::string params = seq.substr(0, seq.size()-1);
    if(final_byte == 'm'){ // SGR
        if(params.empty()) params = "0";
        std::stringstream ss(params); std::string item; std::vector<int> codes;
        while(std::getline(ss, item, ';')){
            try{ codes.push_back(std::stoi(item)); } catch(...) { codes.push_back(0); }
        }
        if(codes.empty()) codes.push_back(0);
        for(int code : codes){
            if(code == 0){
                cur_fg = Color{1,1,1}; cur_bg = Color{0,0,0};
            } else if(code >= 30 && code <= 37){
                cur_fg = ansi_basic_color(code - 30);
            } else if(code >= 40 && code <= 47){
                cur_bg = ansi_basic_color(code - 40);
            } else if(code == 39){ cur_fg = Color{1,1,1}; }
            else if(code == 49){ cur_bg = Color{0,0,0}; }
            // ignore extended colors
        }
    } else if(final_byte == 'H' || final_byte == 'f'){ // cursor position
        int row=1,col=1;
        if(!params.empty()){
            std::stringstream ss(params);
            std::string a,b;
            if(std::getline(ss,a,';')){ if(std::getline(ss,b,';')) col=std::stoi(b); row=std::stoi(a); }
            else row = std::stoi(a);
        }
        cursor_y = std::clamp(row-1, 0, ROWS-1);
        cursor_x = std::clamp(col-1, 0, COLS-1);
    } else if(final_byte == 'J'){
        int p = params.empty()?0:std::stoi(params);
        if(p == 2) clear_screen();
    } else if(final_byte == 'K'){
        int p = params.empty()?0:std::stoi(params);
        if(p == 0) clear_line_from(cursor_y, cursor_x);
        else if(p == 2) clear_line_from(cursor_y, 0);
    }
}

static void process_byte_ansi(char ch){
    if(pstate == PS_NORMAL){
        if(ch == 0x1B) pstate = PS_ESC;
        else put_char_local(ch);
    } else if(pstate == PS_ESC){
        if(ch == '['){ pstate = PS_CSI; esc_buf.clear(); }
        else if(ch == ']'){ pstate = PS_OSC; osc_buf.clear(); }
        else pstate = PS_NORMAL;
    } else if(pstate == PS_CSI){
        esc_buf.push_back(ch);
        unsigned char u = (unsigned char)ch;
        if(u >= 0x40 && u <= 0x7E){
            handle_csi_sequence(esc_buf);
            esc_buf.clear();
            pstate = PS_NORMAL;
        }
    } else if(pstate == PS_OSC){
        if(ch == 0x07){ pstate = PS_NORMAL; osc_buf.clear(); }
        else if(ch == '\\'){ pstate = PS_NORMAL; osc_buf.clear(); }
        else osc_buf.push_back(ch);
    }
}

// ---------- PTY read ----------
static void read_master(){
    if(master_fd < 0) return;
    fd_set rf; FD_ZERO(&rf); FD_SET(master_fd, &rf);
    timeval tv = {0,0};
    int r = select(master_fd+1, &rf, NULL, NULL, &tv);
    if(r > 0 && FD_ISSET(master_fd, &rf)){
        char buf[4096];
        ssize_t n = read(master_fd, buf, sizeof(buf));
        if(n > 0){
            for(ssize_t i=0;i<n;++i) process_byte_ansi(buf[i]);
        } else if(n == 0){
            std::string msg = "[shell closed]";
            for(char c : msg) process_byte_ansi(c);
            process_byte_ansi('\n');
            close(master_fd);
            master_fd = -1;
        }
    }
}

static void send_key_to_pty(const std::string &s){
    if(master_fd < 0) return;
    write(master_fd, s.c_str(), s.size());
}

// ---------- Helper: shell-escape single quotes ----------
static std::string shell_escape_single_quotes(const std::string &s){
    std::string out;
    out.reserve(s.size() + 10);
    for(char c : s){
        if(c == '\''){
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// ---------- AI: run Ollama blocking (safe) ----------
static std::string run_llm_blocking(const std::string& prompt){
    // uses qwen2.5:7b (ollama must be running and model pulled)
    std::string esc = shell_escape_single_quotes(prompt);
    std::string cmd = "echo '" + esc + "' | ollama run qwen2.5:7b 2>/dev/null";
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if(!p) return "[AI error: popen failed]";
    char buf[4096];
    while(fgets(buf, sizeof(buf), p)){
        out += buf;
    }
    int rc = pclose(p);
    (void)rc;
    return out;
}

static void run_llm_async(const std::string &input_line){
    std::thread([input_line](){
        std::string prompt = "You are a shell assistant. Produce a single valid bash command (no explanations, no extra text) that matches the user's request.\nUser request: " + input_line + "\nCommand:";
        std::string out = run_llm_blocking(prompt);
        while(!out.empty() && (out.back()=='\n' || out.back()=='\r' || out.back()==' ' || out.back()=='\t')) out.pop_back();
        std::string cmdline;
        {
            std::istringstream ss(out);
            while(std::getline(ss, cmdline)){
                if(!cmdline.empty()) break;
            }
        }
        if(cmdline.empty()) cmdline = out;
        {
            std::lock_guard<std::mutex> g(ai_mutex);
            ai_result = cmdline;
            ai_ready = true;
        }
    }).detach();
}

// ---------- Input helpers to update shell_buffer and visual line ----------
static void append_to_shell_buffer(char ch){
    shell_buffer.push_back(ch);
    put_char_local(ch); // visual
}
static void shell_backspace(){
    if(!shell_buffer.empty()){
        shell_buffer.pop_back();
        // visual backspace: move cursor back and clear char
        if(cursor_x>0){
            cursor_x--;
            termBuf[cursor_y][cursor_x] = ' ';
            termColor[cursor_y][cursor_x] = cur_fg;
        }
    }
}
static void clear_input_line_visual_and_buffer(){
    // clear visual characters on current line
    for(int c=0;c<COLS;++c) termBuf[cursor_y][c] = ' ';
    // reset cursor_x
    cursor_x = 0;
    shell_buffer.clear();
}

// ---------- Input callbacks ----------
static void char_callback(GLFWwindow*, unsigned int codepoint){
    // codepoint is a Unicode scalar value; we only handle ASCII printable here
    if(input_blocked.load()) return; // ignore while awaiting confirm or blocked

    if(codepoint < 128){
        char ch = (char)codepoint;
        if(ch == '\r' || ch == '\n') return; // handled in key_callback
        if(ch == 0x7f) { shell_backspace(); return; }
        // append to local buffer & visual
        append_to_shell_buffer(ch);
    } else {
        // show ? for non-ascii (simple fallback)
        append_to_shell_buffer('?');
    }
}

static void key_callback(GLFWwindow* window, int key, int, int action, int mods)
{
    if (!(action == GLFW_PRESS || action == GLFW_REPEAT)) return;

    // ============================================================
    // 1. Awaiting confirmation (AI suggestion)
    // ============================================================
    if (awaiting_confirm)
    {
        if (key == GLFW_KEY_Y || key == GLFW_KEY_N)
        {
            bool yes = (key == GLFW_KEY_Y);

            {
                std::lock_guard<std::mutex> lock(ai_mutex);

                if (yes)
                {
                    // -------------------------------
                    // User accepts AI-generated cmd
                    // -------------------------------
                    if (!pending_ai_cmd.empty())
                    {
                        // Send to PTY
                        send_key_to_pty(pending_ai_cmd + "\n");

                        // Clear current visual line
                        clear_input_line_visual_and_buffer();

                        // Print the command visually
                        for (char c : pending_ai_cmd) process_byte_ansi(c);
                        process_byte_ansi('\n');

                        // Note message
                        std::string note = "[AI executed] " + pending_ai_cmd;
                        for (char c : note) process_byte_ansi(c);
                        process_byte_ansi('\n');
                    }
                }
                else
                {
                    // -------------------------------
                    // User CANCELLED AI suggestion
                    // -------------------------------
                    std::string note = "[AI cancelled]";
                    for (char c : note) process_byte_ansi(c);
                    process_byte_ansi('\n');

                    // Send Ctrl+C to reset the prompt
                    send_key_to_pty(std::string(1, 3)); // ASCII 3 == ^C

                    // Visual ^C (optional)
                    process_byte_ansi('^');
                    process_byte_ansi('C');
                    process_byte_ansi('\n');
                }

                pending_ai_cmd.clear();
            }

            awaiting_confirm = false;
            input_blocked.store(false);
            return;
        }

        // Ignore all other keys during confirmation
        return;
    }

    // ============================================================
    // 2. Shift+Enter triggers AI
    // ============================================================
    if (key == GLFW_KEY_ENTER && (mods & GLFW_MOD_SHIFT))
    {
        std::string current_line = shell_buffer;

        size_t p = current_line.find_first_not_of(' ');
        if (p != std::string::npos)
            current_line = current_line.substr(p);

        if (current_line.empty())
        {
            std::string hint = "[AI] Nothing to send (empty line).";
            for (char c : hint) process_byte_ansi(c);
            process_byte_ansi('\n');
            return;
        }

        input_blocked.store(true);

        {
            std::lock_guard<std::mutex> lock(ai_mutex);
            ai_ready = false;
        }

        run_llm_async(current_line);

        std::string msg = "[AI] Thinking...";
        for (char c : msg) process_byte_ansi(c);
        process_byte_ansi('\n');

        return;
    }

    // ============================================================
    // 3. Regular Enter → send shell_buffer to PTY
    // ============================================================
    if (key == GLFW_KEY_ENTER)
    {
        if (!shell_buffer.empty())
        {
            send_key_to_pty(shell_buffer + "\n");
            process_byte_ansi('\n');
            shell_buffer.clear();
            cursor_x = 0;
        }
        else
        {
            send_key_to_pty("\n");
            process_byte_ansi('\n');
        }
        return;
    }

    // ============================================================
    // 4. Backspace
    // ============================================================
    if (key == GLFW_KEY_BACKSPACE)
    {
        shell_backspace();
        return;
    }

    // ============================================================
    // 5. Ctrl+C / Ctrl+D
    // ============================================================
    if (key == GLFW_KEY_C && (mods & GLFW_MOD_CONTROL))
    {
        send_key_to_pty(std::string(1, 3)); // ^C
        return;
    }

    if (key == GLFW_KEY_D && (mods & GLFW_MOD_CONTROL))
    {
        send_key_to_pty(std::string(1, 4)); // ^D
        return;
    }

    // ============================================================
    // 6. Tab
    // ============================================================
    if (key == GLFW_KEY_TAB)
    {
        append_to_shell_buffer('\t');
        return;
    }

    // ============================================================
    // 7. Escape closes window
    // ============================================================
    if (key == GLFW_KEY_ESCAPE)
    {
        glfwSetWindowShouldClose(window, GL_TRUE);
        return;
    }
}

// ---------- Main ----------
int main(int argc, char** argv){
    const char* fontpath = "/usr/share/fonts/TTF/HackNerdFontMono-Regular.ttf";
    if(argc > 1) fontpath = argv[1];

    // initial guesses
    CHAR_W = 10; CHAR_H = 18;
    int win_w = WINDOW_W, win_h = WINDOW_H;
    COLS = win_w / CHAR_W; ROWS = win_h / CHAR_H;
    if(COLS < 10) COLS = 80;
    if(ROWS < 5) ROWS = 24;

    termBuf.resize(ROWS, std::string(COLS, ' '));
    termColor.resize(ROWS, std::vector<Color>(COLS, Color{1,1,1}));

    // spawn PTY + shell
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
    if(pid < 0){ perror("forkpty"); return 1; }
    if(pid == 0){
        const char* shell = getenv("SHELL"); if(!shell) shell="/bin/bash";
        execlp(shell, shell, (char*)NULL);
        _exit(1);
    }
    // non-blocking master
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    // GLFW + GL init
    if(!glfwInit()){ std::cerr<<"glfwInit failed\n"; return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(win_w, win_h, "CerebroShell", NULL, NULL);
    if(!window){ std::cerr<<"glfwCreateWindow failed\n"; return 1; }
    glfwMakeContextCurrent(window);
    glfwSetCharCallback(window, char_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSwapInterval(1);

    if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)){ std::cerr<<"glad init failed\n"; return 1; }
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // GL program and atlas
    programID = build_program();
    glUseProgram(programID);
    uniRes = glGetUniformLocation(programID, "u_resolution");
    uniTex = glGetUniformLocation(programID, "u_tex");

    build_glyph_atlas(fontpath, 18); // build atlas at 18px

    // recompute grid with accurate CHAR_W/CHAR_H
    if(!glyphs.empty()){
        GlyphInfo &gm = glyphs['M' - FIRST_CHAR];
        if(gm.ax > 0) CHAR_W = (int)std::ceil(gm.ax);
        if(gm.bh > 0) CHAR_H = std::max(CHAR_H, (int)gm.bh);
    }
    win_w = WINDOW_W; win_h = WINDOW_H;
    COLS = win_w / CHAR_W; ROWS = win_h / CHAR_H;
    if(COLS < 10) COLS = 80;
    if(ROWS < 5) ROWS = 24;

    termBuf.assign(ROWS, std::string(COLS,' '));
    termColor.assign(ROWS, std::vector<Color>(COLS, Color{1,1,1}));

    // VAO/VBO
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    size_t vertexSize = sizeof(float)*7; // pos2, uv2, col3
    size_t quadBytes = vertexSize * 6;
    size_t totalQuads = (size_t)ROWS * (size_t)COLS + 1;
    glBufferData(GL_ARRAY_BUFFER, totalQuads * quadBytes, NULL, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, vertexSize, (void*)0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, vertexSize, (void*)(sizeof(float)*2)); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, vertexSize, (void*)(sizeof(float)*4)); glEnableVertexAttribArray(2);

    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, atlasTex);
    glUniform1i(uniTex, 0);

    // cursor blink
    double last_blink = 0.0;
    bool cursor_visible = true;

    // main loop
    while(!glfwWindowShouldClose(window)){
        glfwPollEvents();
        read_master();

        // if AI result ready, show suggestion and ask for confirmation
        {
            std::lock_guard<std::mutex> g(ai_mutex);
            if(ai_ready){
                if(!ai_result.empty()){
                    // lock input and ask confirm
                    input_blocked.store(true);
                    pending_ai_cmd = ai_result;
                    awaiting_confirm = true;

                    std::string sug = "[AI suggestion] " + ai_result;
                    for(char c : sug) process_byte_ansi(c);
                    process_byte_ansi('\n');

                    std::string q = "Execute? (y/n)";
                    for(char c : q) process_byte_ansi(c);
                    process_byte_ansi('\n');
                }
                ai_ready = false;
                ai_result.clear();
            }
        }

        // build vertices for entire grid
        std::vector<float> verts;
        verts.reserve((size_t)ROWS*COLS*6*7 + 6*7);

        for(int r=0;r<ROWS;++r){
            for(int c=0;c<COLS;++c){
                unsigned char ch = (unsigned char)termBuf[r][c];
                if(ch < FIRST_CHAR || ch > LAST_CHAR) ch = '?';
                GlyphInfo &gi = glyphs[ch - FIRST_CHAR];

                float x0 = c * (float)CHAR_W;
                float y0 = r * (float)CHAR_H;
                float gx = x0 + gi.bl;
                float gy = y0 + (CHAR_H - gi.bt);
                float gw = gi.bw;
                float gh = gi.bh;
                if(gw <= 0 || gh <= 0) { continue; } // skip empty glyphs

                float s0 = gi.tx;
                float t0 = gi.ty;
                float s1 = s0 + gi.tw;
                float t1 = t0 + gi.th;
                Color &col = termColor[r][c];

                auto pushV = [&](float px,float py,float u,float v,float cr,float cg,float cb){
                    verts.push_back(px); verts.push_back(py);
                    verts.push_back(u); verts.push_back(v);
                    verts.push_back(cr); verts.push_back(cg); verts.push_back(cb);
                };
                pushV(gx,     gy,     s0,t0, col.r,col.g,col.b);
                pushV(gx+gw,  gy,     s1,t0, col.r,col.g,col.b);
                pushV(gx+gw,  gy+gh,  s1,t1, col.r,col.g,col.b);
                pushV(gx+gw,  gy+gh,  s1,t1, col.r,col.g,col.b);
                pushV(gx,     gy+gh,  s0,t1, col.r,col.g,col.b);
                pushV(gx,     gy,     s0,t0, col.r,col.g,col.b);
            }
        }

        // cursor
        double now = glfwGetTime();
        if(now - last_blink >= 0.5){ cursor_visible = !cursor_visible; last_blink = now; }
        if(cursor_visible){
            float x0 = cursor_x * CHAR_W;
            float y0 = cursor_y * CHAR_H;
            float x1 = x0 + CHAR_W;
            float y1 = y0 + CHAR_H;
            auto push = [&](float px,float py){ verts.push_back(px); verts.push_back(py); verts.push_back(0); verts.push_back(0); verts.push_back(1); verts.push_back(1); verts.push_back(1); };
            push(x0,y0); push(x1,y0); push(x1,y1); push(x1,y1); push(x0,y1); push(x0,y0);
        }

        // upload & draw
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        if(!verts.empty()) glBufferSubData(GL_ARRAY_BUFFER, 0, verts.size()*sizeof(float), verts.data());
        // clear & draw
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(programID);
        glUniform2f(uniRes, (float)win_w, (float)win_h);
        glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, atlasTex);
        GLsizei vertexCount = (GLsizei)(verts.size() / 7);
        if(vertexCount > 0) glDrawArrays(GL_TRIANGLES, 0, vertexCount);

        glfwSwapBuffers(window);
    }

    // cleanup
    if(master_fd >= 0) close(master_fd);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

