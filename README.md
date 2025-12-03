CerebroShell – GPU-Accelerated AI-Powered Terminal

A fast, modern, GPU-rendered Linux terminal built in C++ using OpenGL 3.3, FreeType, and PTY, with an integrated AI command assistant powered by Ollama (Qwen2.5-7B).

The terminal renders text using a custom GPU pipeline and assists users by converting natural language queries into executable shell commands — with a safety confirmation step.

<img width="1003" height="631" alt="Screenshot From 2025-12-03 18-09-01" src="https://github.com/user-attachments/assets/11f0b957-37f4-42cb-88b6-1e9f2cd08922" />


🚀 Features
🔹 GPU-Accelerated Rendering

OpenGL 3.3 core

FreeType glyph atlas

Smooth text rendering at high FPS

Custom shader pipeline

🔹 Fully Functional PTY Terminal

Runs your actual system shell (bash/zsh/fish)

Handles ANSI escape codes

Supports cursor movement, colors, clear, etc.

Proper backspace, tab, Ctrl-C, Ctrl-D integration

🔹 AI Command Layer (Ollama + Qwen2.5-7B)

Press Shift + Enter to ask AI for a shell command

Shows the AI suggestion

Requires y/n confirmation

Prevents accidental execution

Helps convert natural language → bash command

🔹 Safe Execution Flow

AI generates a single command (no explanation)

You approve or reject

On rejection, terminal resets via Ctrl-C
🛠️ Build Instructions
Dependencies

Install on Arch/Manjaro:

sudo pacman -S glfw-x11 freetype2 cmake

Clone
git clone https://github.com/Rudra150304/CerebroShell.git
cd CerebroShell

Build
mkdir build
cd build
cmake ..
make -j$(nproc)

Run
./CerebroShell

🤖 AI Setup (Ollama)

Install Ollama:

curl -fsSL https://ollama.com/install.sh | sh


Pull the model:

ollama pull qwen2.5:7b


(You can replace with any model you prefer.)

⌨️ Keybindings
Action	Key
Run shell command	Enter
Ask AI for command	Shift + Enter
Accept AI command	y
Reject AI command	n
Interrupt (send Ctrl-C)	Ctrl + C
EOF	Ctrl + D
Quit	Escape

<img width="1003" height="631" alt="Screenshot From 2025-12-03 18-09-56" src="https://github.com/user-attachments/assets/4f8b5123-e30a-4bb5-b1fc-0f7002b8472d" />


<img width="999" height="619" alt="Screenshot From 2025-12-03 18-14-28" src="https://github.com/user-attachments/assets/478eeacb-7038-4f7e-9066-5698bc22c04b" />




📦 Project Structure




<img width="375" height="174" alt="Screenshot From 2025-12-03 18-25-10" src="https://github.com/user-attachments/assets/fcd82d93-6f13-4e00-a3ae-531dc94d5fb1" />






❤️ Credits

Created by Rudra Pratap Singh
AI-assisted but architected, debugged, and refined with real system-level engineering work.
