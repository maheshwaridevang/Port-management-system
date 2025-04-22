# Port Management System

The Port Management System is a simulation-based logistics scheduler built in C for efficient management of ships at a busy maritime port. It handles ship arrivals, dock assignment, cargo movement using cranes, and controlled undocking with secure authentication, simulating real-world port constraints using message queues, shared memory, and multithreading.

📌 Features
Dynamic handling of regular, emergency, and outgoing ships.

Category-based dock assignment and crane resource management.

Concurrent guessing of secure undocking codes using multithreaded solvers.

Time-step simulation with rule enforcement for dock and cargo operations.

Built with System V IPC and POSIX-compliant C.

🛠️ Requirements
Linux system or Windows Subsystem for Linux (WSL)

GCC compiler with pthread support

Basic knowledge of Linux terminal

🔧 Installation

Clone the repository:
git clone https://github.com/maheshwaridevang/Port-management-system.git
cd Port-management-system

Install dependencies:
sudo apt update
sudo apt install -y build-essential

Compile the scheduler:
gcc -o scheduler.out scheduler.c

Usage
Make sure the input file (e.g., input.txt) is in the path: testcaseX/input.txt.

Run ./validation.out X in one terminal and Start the scheduler by running ./scheduler.out X in another terminal

Replace X with the test case number. Ensure the input file follows the expected format and required IPC keys.

The scheduler will:

Read ship requests via shared memory.

Use message queues to communicate docking, cargo, and undocking operations.

Spawn solver threads to guess radio frequencies for ship undocking.

Simulate actions based on a global time-step.

📂 Project Structure
├── scheduler.c         # Main C file for the scheduler
├── README.md           # Project documentation
└── validation.out       # Folder containing input.txt for each test case


📄 License
This project is licensed under the MIT License.

👤 Author
Maheshwari Devang
GitHub: @maheshwaridevang
