# Collaborative Lock-Free Text Editor

A **real-time collaborative text editor** built entirely with **UNIX system programming** concepts —  
using **Shared Memory (POSIX shm)**, **Named Pipes (FIFOs)**, and **atomic copy-on-write synchronization**.  
It enables multiple users to edit the same document simultaneously **without locks** and ensures **eventual consistency** using a CRDT-inspired *Last Writer Wins* merge strategy.

---

## 📖 Table of Contents

1. [Overview](#overview)
2. [System Design](#system-design)
3. [Key Features](#key-features)
4. [Implementation Details](#implementation-details)
5. [Architecture Diagram](#architecture-diagram)
6. [How to Compile](#how-to-compile)
7. [How to Run](#how-to-run)
8. [Execution Flow](#execution-flow)
9. [Error Handling](#error-handling)
10. [Future Improvements](#future-improvements)
11. [Author](#author)

---

## 🧠 Overview

This project demonstrates a **lock-free multi-user collaborative editor** using pure system-level synchronization.  
Each editor instance runs as a separate process.  
It monitors file changes locally, broadcasts updates via FIFOs, and merges them automatically.

The goal is to **simulate Google Docs-like collaboration** using only *Operating System primitives* — no networking or external libraries.

---

## ⚙️ System Design

Each user runs their own process instance. The components are:

- **Shared Memory Registry** (`/sync_registry`)  
  Tracks active users and their IDs.
  
- **Named Pipes (FIFOs)**  
  Each user has a personal FIFO like `/tmp/pipe_userA` for receiving updates.

- **Listener Thread**  
  Continuously reads updates from its FIFO and stores them in the remote buffer.

- **Local Buffers (Lock-free)**  
  Two vectors — `local_ptr` for outgoing updates and `recv_ptr` for incoming ones — managed using `std::shared_ptr` and `atomic_thread_fence` for copy-on-write semantics.

- **Merge Engine**  
  Periodically merges updates (when threshold = 5) using timestamp-based conflict resolution.

- **Terminal UI**  
  Displays document state, last update time, and notifications of recent updates.

---

## 🧩 Key Features

✅ **Lock-Free Synchronization** — Uses atomic shared pointers (no mutexes).  
✅ **Shared Memory Registry** — Dynamic user discovery and registration.  
✅ **Named Pipe Communication** — Reliable local interprocess communication.  
✅ **Change Detection** — Automatically detects and broadcasts file edits.  
✅ **CRDT-style Merging (LWW)** — Last Writer Wins strategy for conflict resolution.  
✅ **Notification System** — Displays the last 5 received updates.  
✅ **Cross-User Synchronization** — All users eventually converge to the same document state.

---

## 🏗️ Implementation Details

### 🔹 Shared Memory
Used to maintain a registry of active users. Each user process creates or opens `/sync_registry` and adds itself.  
This shared segment allows every instance to know where to send updates.

### 🔹 Named Pipes
Each user has a personal FIFO file (`/tmp/pipe_<user_id>`).  
When a local edit is detected, the user broadcasts it to every other registered user through these FIFOs.

### 🔹 Listener Thread
Each process runs a background thread that continuously reads from its FIFO and appends updates to the shared “remote” buffer.

### 🔹 Lock-Free Buffers
Local and remote updates are managed as **copy-on-write shared pointers**.  
Every modification creates a new snapshot and replaces the old one atomically — enabling **wait-free reads** and **non-blocking writes**.

### 🔹 Conflict Resolution
If two users edit overlapping text regions simultaneously, the conflict is resolved using:
1. Newer timestamps take priority.
2. If timestamps are equal, lexicographically smaller user ID wins.

This ensures deterministic merging and eventual consistency.

### 🔹 Merge Engine
The merge process activates automatically when total pending updates exceed a configurable threshold (default = 5).  
It integrates all buffered edits, rewrites the file, and refreshes the terminal display.

---

## 🧱 Architecture Diagram

```
flowchart TD

    subgraph UserA["User A Process"]
        A1[Local File userA_doc.txt]
        A2[Local Buffer]
        A3[Remote Buffer]
        A4[Listener Thread]
        A5[Merge Engine]
    end

    subgraph UserB["User B Process"]
        B1[Local File userB_doc.txt]
        B2[Local Buffer]
        B3[Remote Buffer]
        B4[Listener Thread]
        B5[Merge Engine]
    end

    R[Shared Memory Registry]
    F1[/tmp/pipe_userA]
    F2[/tmp/pipe_userB]

    A2 -- Broadcast Updates --> F2
    B2 -- Broadcast Updates --> F1
    F1 --> A4
    F2 --> B4
    A4 --> A5
    B4 --> B5
    A5 --> A1
    B5 --> B1
    R <--> A2
    R <--> B2
````

---

## 🧰 How to Compile

You can compile the project using any C++17-compatible compiler (g++, clang++, etc.) with pthreads enabled.

```bash
g++ -std=c++17 CRDT.cpp -o CRDT-lpthread
```

If you’re on Linux, replace `macos` in the filename with `linux` if your file name differs.

---

## 🚀 How to Run

### 🧑‍💻 Step-by-Step Execution

1. **Open multiple terminal windows** — one for each user (e.g., userA, userB, userC).

2. **Run the program for each user:**

   ```bash
   ./CRDT user_1
   ./CRDT user_2
   ```

   Each user:

   * Registers itself in shared memory (`/sync_registry`)
   * Creates a FIFO pipe in `/tmp/pipe_<user_id>`
   * Creates a personal text file (`<user_id>_doc.txt`) if not already present

3. **Observe registration:**
   When a new user joins, all active users display:

   ```
   Registered user: userA
   Active users: userA, userB
   ```

4. **Open user’s document in a text editor (in another terminal):**

   ```bash
   nano userA_doc.txt
   ```

   Modify any line and save.
   The system detects changes automatically.

5. **Real-time synchronization:**

   * When userA edits their file, userB sees the updates appear automatically.
   * The screen refreshes and shows:

     ```
     [Received update from userA] Line 1, "old" → "new"
     [Merging complete] Applied updates.
     ```

6. **Notifications:**
   At the bottom of the terminal, the last 5 received updates are displayed in yellow as:

   ```
   --- Recent Notifications ---
   [Received update from userB] Line 2, "hello" → "world"
   -----------------------------
   ```

7. **Terminate gracefully:**

   * Close the program using `Ctrl+C`.
   * The FIFO and shared memory entries are automatically cleaned up.

---

##  Execution Flow

1. User registers in shared memory.
2. Listener thread starts reading its FIFO.
3. File monitoring thread detects changes via modification timestamps.
4. Local edits are converted to structured updates.
5. Updates are broadcast to all other users.
6. Each recipient merges received updates.
7. Terminal displays updated document and notifications.
8. Process repeats continuously in real-time.

---

## Error Handling

* All system calls (`open`, `mmap`, `shm_open`, `write`) are validated.
* Shared memory is reinitialized automatically if corrupted.
* Non-blocking FIFOs ensure the sender isn’t stuck if a receiver is offline.
* Each failure prints a descriptive error message to the console.
* Automatic cleanup on exit to prevent orphaned shared memory or FIFOs.

---

## Future Improvements

* Implement **socket-based networking** for collaboration across machines.
* Add **fine-grained merging** (character-level instead of line-level).
* Introduce **undo/redo** and **version history tracking**.
* Develop a **GUI or ncurses interface** for enhanced interaction.
* Add **user authentication** and **access control** for multi-user security.

---

## 👨‍💻 Author

**Harsh Jain**
*Operating Systems Project – Collaborative Lock-Free Text Editor*
Built with: MacOS, C++17, POSIX Shared Memory, Named Pipes, pthreads, atomic operations.

---

> “No locks. No servers. Just pure system programming.” 
