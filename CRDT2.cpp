// Compile: g++ -std=c++17 editor_part3_lockfree_macos.cpp -o editor_part3_lockfree_macos -lpthread
// Run: ./editor_part3_lockfree_macos <user_id>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctime>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <sstream>
#include <cerrno>

using namespace std;

// -------------------- Constants --------------------
const char *REGISTRY_SHM = "/sync_registry";
const int MAX_USERS = 5;
const int MERGE_THRESHOLD = 5;
const int MAX_NOTIFICATIONS = 5;

// -------------------- Data Structures --------------------
struct UserInfo
{
    char user_id[32];
};

struct Registry
{
    int user_count;
    UserInfo users[MAX_USERS];
};

struct UpdateObject
{
    char op_type[10]; // "replace"
    int line;
    int start_col;
    int end_col;
    char old_content[256];
    char new_content[256];
    char timestamp[32]; // human readable
    long ts;            // epoch seconds for comparison
    char user_id[32];
};

// -------------------- Globals (lock-free snapshot style) --------------------
// shared_ptr snapshots (copy-on-write). We will use atomic_thread_fence for visibility.
std::shared_ptr<std::vector<UpdateObject>> recv_ptr = std::make_shared<std::vector<UpdateObject>>();
std::shared_ptr<std::vector<UpdateObject>> local_ptr = std::make_shared<std::vector<UpdateObject>>();
std::shared_ptr<std::vector<string>> recent_ptr = std::make_shared<std::vector<string>>();

// printing flag (atomic, used by safe_print)
std::atomic_flag printing = ATOMIC_FLAG_INIT;

// -------------------- Safe Print (Lock-free) --------------------
void safe_print(const string &msg)
{
    while (printing.test_and_set(std::memory_order_acquire))
        std::this_thread::yield();
    cout << msg << endl;
    printing.clear(std::memory_order_release);
}

// -------------------- Helper: append to recent notifications (copy-on-write) ----------
void append_recent_notification(const string &msg)
{
    // copy current snapshot
    atomic_thread_fence(memory_order_acquire);
    auto cur = recent_ptr;
    auto next = std::make_shared<std::vector<string>>(*cur);
    next->push_back(msg);
    if (next->size() > MAX_NOTIFICATIONS)
        next->erase(next->begin());
    // publish
    atomic_thread_fence(memory_order_release);
    recent_ptr = next;
}

// -------------------- Shared Memory (Registry) --------------------
void register_user(const string &user_id)
{
    int shm_fd = shm_open(REGISTRY_SHM, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("shm_open");
        exit(1);
    }

    ftruncate(shm_fd, sizeof(Registry));
    void *ptr = mmap(0, sizeof(Registry), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }

    Registry *registry = (Registry *)ptr;
    if (registry->user_count < 0 || registry->user_count > MAX_USERS)
        registry->user_count = 0;

    bool exists = false;
    for (int i = 0; i < registry->user_count; i++)
    {
        if (strcmp(registry->users[i].user_id, user_id.c_str()) == 0)
            exists = true;
    }

    if (!exists && registry->user_count < MAX_USERS)
    {
        strncpy(registry->users[registry->user_count].user_id, user_id.c_str(), sizeof(registry->users[registry->user_count].user_id)-1);
        registry->users[registry->user_count].user_id[sizeof(registry->users[registry->user_count].user_id)-1] = '\0';
        registry->user_count++;
    }

    stringstream ss;
    ss << "\033[1;36mRegistered user:\033[0m " << user_id << "\nActive users: ";
    for (int i = 0; i < registry->user_count; i++)
    {
        ss << registry->users[i].user_id;
        if (i != registry->user_count - 1)
            ss << ", ";
    }
    safe_print(ss.str());

    munmap(ptr, sizeof(Registry));
    close(shm_fd);
}

// -------------------- File Utilities --------------------
vector<string> read_file(const string &filename)
{
    ifstream file(filename);
    vector<string> lines;
    string line;
    while (getline(file, line))
        lines.push_back(line);
    return lines;
}

void write_initial_file(const string &filename)
{
    const char *INITIAL_DOC[] = {
        "Hello World",
        "This is a collaborative editor",
        "Welcome to SyncText",
        "Edit this document and see real-time updates"};
    ofstream file(filename);
    for (auto &line : INITIAL_DOC)
        file << line << "\n";
    file.close();
}

void write_file_from_lines(const string &filename, const vector<string> &lines)
{
    ofstream file(filename, ios::trunc);
    for (auto &ln : lines)
        file << ln << "\n";
    file.close();
}

void display_file(const string &filename, const vector<string> &lines, const string &last_update)
{
    system("clear");
    cout << "Document: " << filename << endl;
    cout << "Last updated: " << last_update << endl;
    cout << "----------------------------------------" << endl;
    for (int i = 0; i < (int)lines.size(); i++)
        cout << "Line " << i << ": " << lines[i] << endl;
    cout << "----------------------------------------" << endl;

    // snapshot recent notifications and print
    atomic_thread_fence(memory_order_acquire);
    auto recent_snapshot = recent_ptr;
    if (!recent_snapshot->empty())
    {
        cout << "\n--- Recent Notifications ---" << endl;
        for (auto &msg : *recent_snapshot)
            cout << "\033[1;33m" << msg << "\033[0m" << endl;
        cout << "-----------------------------" << endl;
    }

    cout << "Monitoring for changes..." << endl;
}

// -------------------- FIFO Helpers --------------------
string pipe_name(const string &user_id)
{
    return "/tmp/pipe_" + user_id;
}

void create_user_pipe(const string &user_id)
{
    string p = pipe_name(user_id);
    if (mkfifo(p.c_str(), 0666) == -1 && errno != EEXIST)
    {
        perror("mkfifo");
        exit(1);
    }
    safe_print("Pipe created: " + p);
}

void broadcast_update(const UpdateObject &upd, const string &sender_id)
{
    int shm_fd = shm_open(REGISTRY_SHM, O_RDWR, 0666);
    if (shm_fd == -1)
        return;
    void *ptr = mmap(0, sizeof(Registry), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED)
    {
        close(shm_fd);
        return;
    }
    Registry *registry = (Registry *)ptr;

    for (int i = 0; i < registry->user_count; i++)
    {
        string target = registry->users[i].user_id;
        if (target == sender_id)
            continue;
        string p = pipe_name(target);
        int fd = open(p.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd != -1)
        {
            ssize_t w = write(fd, &upd, sizeof(upd));
            if (w == -1)
            {
                // don't crash on write failure; just print once
                safe_print(string("Write failed to ") + p + " : " + strerror(errno));
            }
            close(fd);
        }
    }

    munmap(ptr, sizeof(Registry));
    close(shm_fd);
}

// -------------------- Merge & Apply (CRDT LWW) --------------------
bool ranges_overlap(int a1, int b1, int a2, int b2)
{
    return !(b1 <= a2 || b2 <= a1);
}

void merge_and_apply(vector<UpdateObject> local_ops, const string &user_id)
{
    string filename = user_id + "_doc.txt";
    vector<string> doc = read_file(filename);

    // atomically grab and clear recv buffer (copy-on-write)
    atomic_thread_fence(memory_order_acquire);
    auto recv_snapshot = recv_ptr;
    // publish empty buffer
    atomic_thread_fence(memory_order_release);
    recv_ptr = std::make_shared<std::vector<UpdateObject>>();

    // combine
    vector<UpdateObject> all = local_ops;
    all.insert(all.end(), recv_snapshot->begin(), recv_snapshot->end());

    if (all.empty())
        return;

    int max_line = -1;
    for (auto &u : all)
        if (u.line > max_line)
            max_line = u.line;
    while ((int)doc.size() <= max_line)
        doc.push_back("");

    int n = all.size();
    vector<bool> keep(n, true);

    for (int i = 0; i < n; ++i)
    {
        if (!keep[i]) continue;
        for (int j = i + 1; j < n; ++j)
        {
            if (!keep[j]) continue;
            if (all[i].line == all[j].line &&
                ranges_overlap(all[i].start_col, all[i].end_col, all[j].start_col, all[j].end_col))
            {
                if (all[i].ts > all[j].ts)
                    keep[j] = false;
                else if (all[i].ts < all[j].ts)
                {
                    keep[i] = false;
                    break;
                }
                else
                {
                    if (strcmp(all[i].user_id, all[j].user_id) <= 0)
                        keep[j] = false;
                    else
                    {
                        keep[i] = false;
                        break;
                    }
                }
            }
        }
    }

    unordered_map<int, vector<UpdateObject>> updates_by_line;
    for (int i = 0; i < n; ++i)
        if (keep[i])
            updates_by_line[all[i].line].push_back(all[i]);

    for (auto &kv : updates_by_line)
    {
        int line_no = kv.first;
        auto ops = kv.second;
        sort(ops.begin(), ops.end(), [](const UpdateObject &a, const UpdateObject &b)
             { return a.start_col > b.start_col; });

        string base = doc[line_no];
        for (auto &op : ops)
        {
            int sc = max(0, op.start_col);
            int ec = max(0, op.end_col);
            if (sc > (int)base.size()) sc = base.size();
            if (ec > (int)base.size()) ec = base.size();
            string left = base.substr(0, sc);
            string right = (ec < (int)base.size()) ? base.substr(ec) : "";
            base = left + string(op.new_content) + right;
        }
        doc[line_no] = base;
    }

    write_file_from_lines(filename, doc);

    time_t now = time(0);
    string dt = ctime(&now);
    if (!dt.empty() && dt.back() == '\n') dt.pop_back();
    display_file(filename, doc, dt);

    safe_print("\033[1;35m[Merging complete]\033[0m Applied updates.");
}

// -------------------- Merge Trigger (uses snapshots) --------------------
void try_merge_if_needed(const string &user_id, const vector<UpdateObject> &local_ops_for_merge = {})
{
    atomic_thread_fence(memory_order_acquire);
    auto recv_snapshot = recv_ptr;
    auto local_snapshot = local_ptr;

    size_t total = recv_snapshot->size() + local_snapshot->size() + local_ops_for_merge.size();

    if (total >= MERGE_THRESHOLD)
    {
        // prepare merge vector
        vector<UpdateObject> to_merge = local_ops_for_merge;
        to_merge.insert(to_merge.end(), local_snapshot->begin(), local_snapshot->end());
        // clear local snapshot
        atomic_thread_fence(memory_order_release);
        local_ptr = std::make_shared<std::vector<UpdateObject>>();
        merge_and_apply(to_merge, user_id);
    }
}

// -------------------- Listener Thread --------------------
void listener_thread(const string &user_id)
{
    string p = pipe_name(user_id);
    int fd = open(p.c_str(), O_RDONLY);
    if (fd == -1)
    {
        perror("open listener");
        return;
    }

    UpdateObject upd;
    while (true)
    {
        ssize_t n = read(fd, &upd, sizeof(upd));
        if (n > 0)
        {
            // append to recv_ptr (copy-on-write)
            atomic_thread_fence(memory_order_acquire);
            auto cur = recv_ptr;
            auto next = std::make_shared<std::vector<UpdateObject>>(*cur);
            next->push_back(upd);
            atomic_thread_fence(memory_order_release);
            recv_ptr = next;

            string msg = "[Received update from " + string(upd.user_id) +
                         "] Line " + to_string(upd.line) +
                         ", cols " + to_string(upd.start_col) + "-" + to_string(upd.end_col) +
                         ", \"" + string(upd.old_content) + "\" → \"" + string(upd.new_content) +
                         "\" @ " + string(upd.timestamp);

            // append to recent notifications (copy-on-write)
            append_recent_notification(msg);

            safe_print("\033[1;32m" + msg + "\033[0m");

            try_merge_if_needed(user_id);
        }
        else
        {
            this_thread::sleep_for(chrono::milliseconds(100));
        }
    }
}

// -------------------- Change Detection (improved) --------------------
void detect_changes(vector<string> &old_lines, const vector<string> &new_lines, const string &user_id)
{
    int old_n = (int)old_lines.size();
    int new_n = (int)new_lines.size();
    int max_n = max(old_n, new_n);

    for (int i = 0; i < max_n; ++i)
    {
        string old_line = (i < old_n) ? old_lines[i] : "";
        string new_line = (i < new_n) ? new_lines[i] : "";
        if (old_line == new_line) continue;

        int start_col = 0;
        int minlen = min((int)old_line.size(), (int)new_line.size());
        while (start_col < minlen && old_line[start_col] == new_line[start_col]) start_col++;

        int old_end = (int)old_line.size();
        int new_end = (int)new_line.size();
        while (old_end - 1 >= start_col && new_end - 1 >= start_col &&
               old_line[old_end - 1] == new_line[new_end - 1])
        {
            old_end--; new_end--;
        }

        string old_part = (start_col < old_end) ? old_line.substr(start_col, old_end - start_col) : string("");
        string new_part = (start_col < new_end) ? new_line.substr(start_col, new_end - start_col) : string("");
        if (old_part == new_part) continue;

        UpdateObject upd{};
        strncpy(upd.op_type, "replace", sizeof(upd.op_type)-1);
        upd.line = i;
        upd.start_col = start_col;
        upd.end_col = max(old_end, new_end);
        strncpy(upd.old_content, old_part.c_str(), sizeof(upd.old_content)-1);
        strncpy(upd.new_content, new_part.c_str(), sizeof(upd.new_content)-1);
        strncpy(upd.user_id, user_id.c_str(), sizeof(upd.user_id)-1);
        time_t now = time(nullptr);
        upd.ts = (long)now;
        strncpy(upd.timestamp, ctime(&now), sizeof(upd.timestamp)-1);
        if (strlen(upd.timestamp)) upd.timestamp[strcspn(upd.timestamp, "\n")] = '\0';

        safe_print("\033[1;34m[Local Change Detected]\033[0m Line " + to_string(i) +
                   ", \"" + old_part + "\" → \"" + new_part + "\"");

        // append to local_ptr (copy-on-write)
        atomic_thread_fence(memory_order_acquire);
        auto cur = local_ptr;
        auto next = std::make_shared<std::vector<UpdateObject>>(*cur);
        next->push_back(upd);
        atomic_thread_fence(memory_order_release);
        local_ptr = next;

        if (next->size() >= MERGE_THRESHOLD)
        {
            // broadcast all
            vector<UpdateObject> to_send = *next;
            atomic_thread_fence(memory_order_release);
            local_ptr = std::make_shared<std::vector<UpdateObject>>();
            safe_print("\033[1;36m[Broadcasting updates...]\033[0m");
            for (auto &u : to_send)
                broadcast_update(u, user_id);
            try_merge_if_needed(user_id, to_send);
        }
        else
        {
            try_merge_if_needed(user_id);
        }
    }

    old_lines = new_lines;
}

// -------------------- Main --------------------
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        cerr << "Usage: ./editor_part3_lockfree_macos <user_id>\n";
        return 1;
    }

    string user_id = argv[1];
    register_user(user_id);
    create_user_pipe(user_id);

    thread listener(listener_thread, user_id);

    string filename = user_id + "_doc.txt";
    if (access(filename.c_str(), F_OK) == -1)
        write_initial_file(filename);

    vector<string> old_content = read_file(filename);
    struct stat file_stat;
    stat(filename.c_str(), &file_stat);
    time_t last_mod_time = file_stat.st_mtime;

    while (true)
    {
        stat(filename.c_str(), &file_stat);
        if (file_stat.st_mtime != last_mod_time)
        {
            last_mod_time = file_stat.st_mtime;
            vector<string> new_content = read_file(filename);
            time_t now = time(0);
            string dt = ctime(&now);
            if (!dt.empty() && dt.back() == '\n') dt.pop_back();
            display_file(filename, new_content, dt);
            detect_changes(old_content, new_content, user_id);
        }
        this_thread::sleep_for(chrono::seconds(2));
    }

    listener.join();
    unlink(pipe_name(user_id).c_str());
    return 0;
}
