// Compile: g++ -std=c++17 -O2 CRDT2.cpp -o CRDT -lpthread
// Run:     ./CRDT <user_id>
//          ./CRDT --poll <user_id>     # legacy 2s stat() watch
//          ./CRDT --bench              # A/B microbenchmarks + converge test

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <ctime>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <sstream>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <iomanip>
#include <functional>
#include <iterator>

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#endif

using namespace std;

// -------------------- Constants --------------------
const char *DEFAULT_REGISTRY_SHM = "/sync_registry";
const int MAX_USERS = 5;
const int MERGE_THRESHOLD = 5;
const int MAX_NOTIFICATIONS = 5;
const int POLL_INTERVAL_MS = 2000;
const int LEGACY_FIFO_SLEEP_MS = 100;
const size_t REGISTRY_MAP_SIZE = 4096;

const char *g_registry_shm = DEFAULT_REGISTRY_SHM;

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
    char timestamp[32];
    long ts; // epoch nanoseconds for LWW
    char user_id[32];
};

using UpdateVec = vector<UpdateObject>;
using StrVec = vector<string>;

// -------------------- Globals --------------------
shared_ptr<UpdateVec> recv_ptr = make_shared<UpdateVec>();
shared_ptr<UpdateVec> local_ptr = make_shared<UpdateVec>();
shared_ptr<StrVec> recent_ptr = make_shared<StrVec>();

atomic_flag printing = ATOMIC_FLAG_INIT;
atomic<bool> g_running{true};
atomic<bool> g_quiet{false};
atomic<bool> g_headless{false};
atomic<bool> g_use_kqueue{true};
atomic<int> g_listener_read_fd{-1};
atomic<int> g_listener_dummy_fd{-1};
atomic<long> g_bytes_sent{0};
atomic<long> g_ipc_recv_ns{0};
atomic<int> g_ipc_recv_count{0};
atomic<bool> g_apply_merges{true};

string g_self_uid;

// -------------------- Time --------------------
long realtime_ns()
{
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

long monotonic_ns()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

string human_timestamp()
{
    time_t now = time(nullptr);
    string dt = ctime(&now);
    if (!dt.empty() && dt.back() == '\n')
        dt.pop_back();
    return dt;
}

// -------------------- Safe Print --------------------
void safe_print(const string &msg)
{
    if (g_quiet.load(memory_order_relaxed))
        return;
    while (printing.test_and_set(memory_order_acquire))
        this_thread::yield();
    cout << msg << endl;
    printing.clear(memory_order_release);
}

// -------------------- Atomic COW snapshots (C++17 shared_ptr atomics) --------------------
void cow_append_recent(const string &msg)
{
    shared_ptr<StrVec> cur, next;
    do
    {
        cur = atomic_load_explicit(&recent_ptr, memory_order_acquire);
        next = make_shared<StrVec>(*cur);
        next->push_back(msg);
        if ((int)next->size() > MAX_NOTIFICATIONS)
            next->erase(next->begin());
    } while (!atomic_compare_exchange_weak_explicit(&recent_ptr, &cur, next,
                                                    memory_order_release, memory_order_acquire));
}

void cow_append_update(shared_ptr<UpdateVec> *slot, const UpdateObject &upd)
{
    shared_ptr<UpdateVec> cur, next;
    do
    {
        cur = atomic_load_explicit(slot, memory_order_acquire);
        next = make_shared<UpdateVec>(*cur);
        next->push_back(upd);
    } while (!atomic_compare_exchange_weak_explicit(slot, &cur, next,
                                                    memory_order_release, memory_order_acquire));
}

shared_ptr<UpdateVec> cow_take(shared_ptr<UpdateVec> *slot)
{
    auto empty = make_shared<UpdateVec>();
    return atomic_exchange_explicit(slot, empty, memory_order_acq_rel);
}

size_t cow_size(const shared_ptr<UpdateVec> *slot)
{
    auto snap = atomic_load_explicit(slot, memory_order_acquire);
    return snap->size();
}

// -------------------- Shared Memory Registry --------------------
string registry_lock_path()
{
    return string("/tmp/crdt_lock_") + (g_registry_shm + 1); // skip leading '/'
}

int lock_registry(bool exclusive)
{
    int fd = open(registry_lock_path().c_str(), O_CREAT | O_RDWR, 0666);
    if (fd == -1)
        return -1;
    if (flock(fd, exclusive ? LOCK_EX : LOCK_SH) == -1)
    {
        close(fd);
        return -1;
    }
    return fd;
}

void unlock_registry(int fd)
{
    if (fd != -1)
    {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

Registry *map_registry(int *out_fd, bool create)
{
    int shm_fd = shm_open(g_registry_shm, create ? (O_CREAT | O_RDWR) : O_RDWR, 0666);
    if (shm_fd == -1)
        return nullptr;
    struct stat st {};
    if (fstat(shm_fd, &st) == -1)
    {
        close(shm_fd);
        return nullptr;
    }
    if (st.st_size < (off_t)sizeof(Registry))
    {
        if (ftruncate(shm_fd, (off_t)4096) == -1 && st.st_size == 0)
        {
            close(shm_fd);
            return nullptr;
        }
        if (fstat(shm_fd, &st) == -1 || st.st_size < (off_t)sizeof(Registry))
        {
            close(shm_fd);
            return nullptr;
        }
    }
    size_t map_len = (size_t)st.st_size;
    if (map_len < 4096)
        map_len = 4096;
    void *ptr = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED)
    {
        close(shm_fd);
        return nullptr;
    }
    *out_fd = shm_fd;
    return (Registry *)ptr;
}

void unmap_registry(void *ptr, int shm_fd)
{
    if (ptr && ptr != MAP_FAILED)
    {
        struct stat st {};
        size_t n = REGISTRY_MAP_SIZE;
        if (shm_fd != -1 && fstat(shm_fd, &st) == 0 && st.st_size > 0)
            n = (size_t)st.st_size;
        munmap(ptr, n);
    }
    if (shm_fd != -1)
        close(shm_fd);
}

void register_user(const string &user_id)
{
    int lk = lock_registry(true);
    int shm_fd = -1;
    Registry *registry = map_registry(&shm_fd, true);
    if (!registry)
    {
        unlock_registry(lk);
        perror("shm registry");
        exit(1);
    }
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
        strncpy(registry->users[registry->user_count].user_id, user_id.c_str(),
                sizeof(registry->users[registry->user_count].user_id) - 1);
        registry->users[registry->user_count].user_id[sizeof(registry->users[registry->user_count].user_id) - 1] = '\0';
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
    unmap_registry(registry, shm_fd);
    unlock_registry(lk);
}

void unregister_user(const string &user_id)
{
    int lk = lock_registry(true);
    int shm_fd = -1;
    Registry *registry = map_registry(&shm_fd, false);
    if (!registry)
    {
        unlock_registry(lk);
        return;
    }
    int w = 0;
    for (int i = 0; i < registry->user_count; i++)
    {
        if (strcmp(registry->users[i].user_id, user_id.c_str()) == 0)
            continue;
        registry->users[w++] = registry->users[i];
    }
    registry->user_count = w;
    unmap_registry(registry, shm_fd);
    unlock_registry(lk);
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
}

void write_file_from_lines(const string &filename, const vector<string> &lines)
{
    ofstream file(filename, ios::trunc);
    for (auto &ln : lines)
        file << ln << "\n";
}

void display_file(const string &filename, const vector<string> &lines, const string &last_update)
{
    if (g_headless.load(memory_order_relaxed))
        return;
    system("clear");
    cout << "Document: " << filename << endl;
    cout << "Last updated: " << last_update << endl;
    cout << "----------------------------------------" << endl;
    for (int i = 0; i < (int)lines.size(); i++)
        cout << "Line " << i << ": " << lines[i] << endl;
    cout << "----------------------------------------" << endl;

    auto recent_snapshot = atomic_load_explicit(&recent_ptr, memory_order_acquire);
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

bool write_full(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n)
    {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;
            return false;
        }
        if (w == 0)
            return false;
        off += (size_t)w;
        g_bytes_sent.fetch_add((long)w, memory_order_relaxed);
    }
    return true;
}

bool read_full(int fd, void *buf, size_t n, bool nonblock_retry)
{
    char *p = (char *)buf;
    size_t off = 0;
    while (off < n && g_running.load(memory_order_relaxed))
    {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (!nonblock_retry)
                    return false;
                this_thread::sleep_for(chrono::milliseconds(LEGACY_FIFO_SLEEP_MS));
                continue;
            }
            return false;
        }
        if (r == 0)
        {
            if (nonblock_retry)
            {
                this_thread::sleep_for(chrono::milliseconds(LEGACY_FIFO_SLEEP_MS));
                continue;
            }
            return false;
        }
        off += (size_t)r;
    }
    return off == n;
}

void broadcast_updates(const vector<UpdateObject> &upds, const string &sender_id)
{
    if (upds.empty())
        return;
    int lk = lock_registry(false);
    int shm_fd = -1;
    Registry *registry = map_registry(&shm_fd, false);
    if (!registry)
    {
        unlock_registry(lk);
        return;
    }

    for (int i = 0; i < registry->user_count; i++)
    {
        string target = registry->users[i].user_id;
        if (target == sender_id)
            continue;
        string p = pipe_name(target);
        int fd = open(p.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd != -1)
        {
            if (!write_full(fd, upds.data(), upds.size() * sizeof(UpdateObject)))
                safe_print(string("Write failed to ") + p + " : " + strerror(errno));
            close(fd);
        }
    }

    unmap_registry(registry, shm_fd);
    unlock_registry(lk);
}

void broadcast_update(const UpdateObject &upd, const string &sender_id)
{
    vector<UpdateObject> one{upd};
    broadcast_updates(one, sender_id);
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

    auto recv_snapshot = cow_take(&recv_ptr);

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

    int n = (int)all.size();
    vector<bool> keep(n, true);

    for (int i = 0; i < n; ++i)
    {
        if (!keep[i])
            continue;
        for (int j = i + 1; j < n; ++j)
        {
            if (!keep[j])
                continue;
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
            if (sc > (int)base.size())
                sc = (int)base.size();
            if (ec > (int)base.size())
                ec = (int)base.size();
            string left = base.substr(0, sc);
            string right = (ec < (int)base.size()) ? base.substr(ec) : "";
            base = left + string(op.new_content) + right;
        }
        doc[line_no] = base;
    }

    write_file_from_lines(filename, doc);
    display_file(filename, doc, human_timestamp());
    safe_print("\033[1;35m[Merging complete]\033[0m Applied updates.");
}

void try_merge_if_needed(const string &user_id, const vector<UpdateObject> &local_ops_for_merge = {})
{
    size_t total = cow_size(&recv_ptr) + cow_size(&local_ptr) + local_ops_for_merge.size();
    if (total >= (size_t)MERGE_THRESHOLD)
    {
        vector<UpdateObject> to_merge = local_ops_for_merge;
        auto local_snapshot = cow_take(&local_ptr);
        to_merge.insert(to_merge.end(), local_snapshot->begin(), local_snapshot->end());
        merge_and_apply(to_merge, user_id);
    }
}

// -------------------- Listener --------------------
int open_fifo_reader(const string &path, bool blocking_dummy)
{
    if (blocking_dummy)
    {
        int rd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (rd == -1)
            return -1;
        int dummy = open(path.c_str(), O_WRONLY);
        if (dummy == -1)
        {
            close(rd);
            return -1;
        }
        int flags = fcntl(rd, F_GETFL, 0);
        fcntl(rd, F_SETFL, flags & ~O_NONBLOCK);
        g_listener_dummy_fd.store(dummy, memory_order_relaxed);
        return rd;
    }
    return open(path.c_str(), O_RDONLY | O_NONBLOCK);
}

void listener_thread(const string &user_id, bool blocking_dummy)
{
    string p = pipe_name(user_id);
    int fd = open_fifo_reader(p, blocking_dummy);
    if (fd == -1)
    {
        perror("open listener");
        return;
    }
    g_listener_read_fd.store(fd, memory_order_relaxed);

    UpdateObject upd;
    while (g_running.load(memory_order_relaxed))
    {
        if (!read_full(fd, &upd, sizeof(upd), !blocking_dummy))
        {
            if (!g_running.load(memory_order_relaxed))
                break;
            if (blocking_dummy)
                break;
            continue;
        }

        cow_append_update(&recv_ptr, upd);
        g_ipc_recv_ns.store(monotonic_ns(), memory_order_release);
        g_ipc_recv_count.fetch_add(1, memory_order_acq_rel);

        string msg = "[Received update from " + string(upd.user_id) +
                     "] Line " + to_string(upd.line) +
                     ", cols " + to_string(upd.start_col) + "-" + to_string(upd.end_col) +
                     ", \"" + string(upd.old_content) + "\" → \"" + string(upd.new_content) +
                     "\" @ " + string(upd.timestamp);
        cow_append_recent(msg);
        safe_print("\033[1;32m" + msg + "\033[0m");
        if (g_apply_merges.load(memory_order_relaxed))
            try_merge_if_needed(user_id);
    }

    close(fd);
    int dummy = g_listener_dummy_fd.exchange(-1);
    if (dummy != -1)
        close(dummy);
    g_listener_read_fd.store(-1, memory_order_relaxed);
}

// -------------------- Change Detection --------------------
bool extract_replace_span(const string &old_line, const string &new_line,
                          int &start_col, int &end_col, string &old_part, string &new_part)
{
    if (old_line == new_line)
        return false;
    start_col = 0;
    int minlen = min((int)old_line.size(), (int)new_line.size());
    while (start_col < minlen && old_line[start_col] == new_line[start_col])
        start_col++;

    int old_end = (int)old_line.size();
    int new_end = (int)new_line.size();
    while (old_end - 1 >= start_col && new_end - 1 >= start_col &&
           old_line[old_end - 1] == new_line[new_end - 1])
    {
        old_end--;
        new_end--;
    }

    old_part = (start_col < old_end) ? old_line.substr(start_col, old_end - start_col) : string("");
    new_part = (start_col < new_end) ? new_line.substr(start_col, new_end - start_col) : string("");
    if (old_part == new_part)
        return false;
    end_col = max(old_end, new_end);
    return true;
}

void stamp_update(UpdateObject &upd, const string &user_id)
{
    strncpy(upd.user_id, user_id.c_str(), sizeof(upd.user_id) - 1);
    upd.ts = realtime_ns();
    string ht = human_timestamp();
    strncpy(upd.timestamp, ht.c_str(), sizeof(upd.timestamp) - 1);
}

void detect_changes(vector<string> &old_lines, const vector<string> &new_lines, const string &user_id)
{
    int old_n = (int)old_lines.size();
    int new_n = (int)new_lines.size();
    int max_n = max(old_n, new_n);

    for (int i = 0; i < max_n; ++i)
    {
        string old_line = (i < old_n) ? old_lines[i] : "";
        string new_line = (i < new_n) ? new_lines[i] : "";
        int start_col = 0, end_col = 0;
        string old_part, new_part;
        if (!extract_replace_span(old_line, new_line, start_col, end_col, old_part, new_part))
            continue;

        UpdateObject upd{};
        strncpy(upd.op_type, "replace", sizeof(upd.op_type) - 1);
        upd.line = i;
        upd.start_col = start_col;
        upd.end_col = end_col;
        strncpy(upd.old_content, old_part.c_str(), sizeof(upd.old_content) - 1);
        strncpy(upd.new_content, new_part.c_str(), sizeof(upd.new_content) - 1);
        stamp_update(upd, user_id);

        safe_print("\033[1;34m[Local Change Detected]\033[0m Line " + to_string(i) +
                   ", \"" + old_part + "\" → \"" + new_part + "\"");

        cow_append_update(&local_ptr, upd);
        auto snap = atomic_load_explicit(&local_ptr, memory_order_acquire);
        if ((int)snap->size() >= MERGE_THRESHOLD)
        {
            auto taken = cow_take(&local_ptr);
            vector<UpdateObject> to_send = *taken;
            safe_print("\033[1;36m[Broadcasting updates...]\033[0m");
            broadcast_updates(to_send, user_id);
            try_merge_if_needed(user_id, to_send);
        }
        else
        {
            try_merge_if_needed(user_id);
        }
    }

    old_lines = new_lines;
}

// -------------------- File watch (kqueue / poll) --------------------
void mark_ready(const string &user_id);

void handle_file_event(const string &filename, const string &user_id,
                       vector<string> &old_content, time_t &last_mod_time)
{
    struct stat file_stat {};
    if (stat(filename.c_str(), &file_stat) != 0)
        return;
    last_mod_time = file_stat.st_mtime;
    vector<string> new_content = read_file(filename);
    if (new_content == old_content)
        return;
    display_file(filename, new_content, human_timestamp());
    detect_changes(old_content, new_content, user_id);
}

#ifdef __APPLE__
int kqueue_register_file(int kq, int *watch_fd, const string &filename)
{
    if (*watch_fd != -1)
        close(*watch_fd);
#ifdef O_EVTONLY
    *watch_fd = open(filename.c_str(), O_EVTONLY);
#else
    *watch_fd = open(filename.c_str(), O_RDONLY);
#endif
    if (*watch_fd == -1)
        return -1;
    struct kevent kev {};
    EV_SET(&kev, *watch_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE,
           0, NULL);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1)
        return -1;
    return 0;
}

void run_kqueue_watch(const string &filename, const string &user_id, vector<string> &old_content)
{
    int kq = kqueue();
    if (kq == -1)
    {
        perror("kqueue");
        return;
    }
    int watch_fd = -1;
    if (kqueue_register_file(kq, &watch_fd, filename) == -1)
    {
        perror("kqueue register");
        close(kq);
        return;
    }
    mark_ready(user_id);
    struct stat st {};
    stat(filename.c_str(), &st);
    time_t last_mod_time = st.st_mtime;

    while (g_running.load(memory_order_relaxed))
    {
        struct kevent ev {};
        struct timespec timeout {
            0, 200000000
        };
        int n = kevent(kq, NULL, 0, &ev, 1, &timeout);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
        {
            handle_file_event(filename, user_id, old_content, last_mod_time);
            continue;
        }

        unsigned fflags = ev.fflags;
        if (fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))
        {
            this_thread::sleep_for(chrono::milliseconds(20));
            kqueue_register_file(kq, &watch_fd, filename);
        }
        this_thread::sleep_for(chrono::milliseconds(5)); // coalesce bursts
        handle_file_event(filename, user_id, old_content, last_mod_time);
    }
    if (watch_fd != -1)
        close(watch_fd);
    close(kq);
}
#endif

void run_poll_watch(const string &filename, const string &user_id, vector<string> &old_content)
{
    struct stat file_stat {};
    stat(filename.c_str(), &file_stat);
    time_t last_mod_time = file_stat.st_mtime;
    mark_ready(user_id);
    while (g_running.load(memory_order_relaxed))
    {
        int slept = 0;
        while (slept < POLL_INTERVAL_MS && g_running.load(memory_order_relaxed))
        {
            this_thread::sleep_for(chrono::milliseconds(50));
            slept += 50;
        }
        if (!g_running.load(memory_order_relaxed))
            break;
        stat(filename.c_str(), &file_stat);
        if (file_stat.st_mtime != last_mod_time)
            handle_file_event(filename, user_id, old_content, last_mod_time);
    }
}

void monitor_loop(const string &user_id)
{
    string filename = user_id + "_doc.txt";
    if (access(filename.c_str(), F_OK) == -1)
        write_initial_file(filename);
    vector<string> old_content = read_file(filename);

#ifdef __APPLE__
    if (g_use_kqueue.load(memory_order_relaxed))
        run_kqueue_watch(filename, user_id, old_content);
    else
#endif
        run_poll_watch(filename, user_id, old_content);
}

// -------------------- Cleanup --------------------
void cleanup_process()
{
    g_running.store(false, memory_order_release);
    int dummy = g_listener_dummy_fd.exchange(-1);
    if (dummy != -1)
        close(dummy);
    int rd = g_listener_read_fd.exchange(-1);
    if (rd != -1)
        close(rd);
    if (!g_self_uid.empty())
    {
        unregister_user(g_self_uid);
        unlink(pipe_name(g_self_uid).c_str());
        unlink(("/tmp/crdt_ready_" + g_self_uid).c_str());
    }
}

void on_signal(int)
{
    cleanup_process();
}

void mark_ready(const string &user_id)
{
    ofstream f("/tmp/crdt_ready_" + user_id);
    f << "ready\n";
}

void run_editor(const string &user_id)
{
    g_self_uid = user_id;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    register_user(user_id);
    create_user_pipe(user_id);
    thread listener(listener_thread, user_id, true);
    monitor_loop(user_id);
    cleanup_process();
    listener.join();
}

// -------------------- Benchmark helpers --------------------
struct Stats
{
    int n = 0;
    double mean_us = 0;
    double p50_us = 0;
    double p99_us = 0;
    double min_us = 0;
    double max_us = 0;
};

double percentile_us(vector<double> v, double p)
{
    if (v.empty())
        return 0;
    sort(v.begin(), v.end());
    double idx = (p / 100.0) * (v.size() - 1);
    size_t lo = (size_t)floor(idx);
    size_t hi = (size_t)ceil(idx);
    if (hi >= v.size())
        hi = v.size() - 1;
    double w = idx - lo;
    return v[lo] * (1.0 - w) + v[hi] * w;
}

Stats summarize(const vector<double> &ns_samples)
{
    Stats s;
    s.n = (int)ns_samples.size();
    if (s.n == 0)
        return s;
    vector<double> us;
    us.reserve(ns_samples.size());
    double sum = 0;
    for (double x : ns_samples)
    {
        double u = x / 1000.0;
        us.push_back(u);
        sum += u;
    }
    s.mean_us = sum / s.n;
    s.p50_us = percentile_us(us, 50);
    s.p99_us = percentile_us(us, 99);
    s.min_us = *min_element(us.begin(), us.end());
    s.max_us = *max_element(us.begin(), us.end());
    return s;
}

string fmt_us(double us)
{
    ostringstream o;
    o << fixed << setprecision(1);
    if (us >= 1000000.0)
        o << (us / 1000000.0) << " s";
    else if (us >= 1000.0)
        o << (us / 1000.0) << " ms";
    else
        o << us << " us";
    return o.str();
}

void print_stats(const string &name, const Stats &s)
{
    cout << "  " << name << ": n=" << s.n
         << "  mean=" << fmt_us(s.mean_us)
         << "  p50=" << fmt_us(s.p50_us)
         << "  p99=" << fmt_us(s.p99_us)
         << "  min=" << fmt_us(s.min_us)
         << "  max=" << fmt_us(s.max_us) << "\n";
}

bool wait_until(const function<bool()> &pred, int timeout_ms)
{
    auto t0 = chrono::steady_clock::now();
    while (chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count() < timeout_ms)
    {
        if (pred())
            return true;
        this_thread::sleep_for(chrono::milliseconds(2));
    }
    return pred();
}

UpdateObject make_op(int line, const string &oldc, const string &newc, const string &uid)
{
    UpdateObject upd{};
    strncpy(upd.op_type, "replace", sizeof(upd.op_type) - 1);
    upd.line = line;
    upd.start_col = 0;
    upd.end_col = (int)oldc.size();
    strncpy(upd.old_content, oldc.c_str(), sizeof(upd.old_content) - 1);
    strncpy(upd.new_content, newc.c_str(), sizeof(upd.new_content) - 1);
    stamp_update(upd, uid);
    return upd;
}

// Isolated detect-latency watch (does not run CRDT merge).
void bench_detect_watch(const string &filename, atomic<long> *seen_ns, atomic<bool> *stop, bool use_kq)
{
#ifdef __APPLE__
    if (use_kq)
    {
        int kq = kqueue();
        int watch_fd = -1;
        if (kq == -1 || kqueue_register_file(kq, &watch_fd, filename) == -1)
            return;
        while (!stop->load(memory_order_relaxed))
        {
            struct kevent ev {};
            struct timespec timeout {
                0, 50000000
            };
            int n = kevent(kq, NULL, 0, &ev, 1, &timeout);
            if (n > 0)
                seen_ns->store(monotonic_ns(), memory_order_release);
        }
        if (watch_fd != -1)
            close(watch_fd);
        close(kq);
        return;
    }
#else
    (void)use_kq;
#endif
    struct stat st {};
    stat(filename.c_str(), &st);
    time_t last = st.st_mtime;
    while (!stop->load(memory_order_relaxed))
    {
        this_thread::sleep_for(chrono::milliseconds(POLL_INTERVAL_MS));
        if (stat(filename.c_str(), &st) == 0 && st.st_mtime != last)
        {
            last = st.st_mtime;
            seen_ns->store(monotonic_ns(), memory_order_release);
        }
    }
}

Stats bench_detect(bool use_kq, int trials)
{
    string path = "/tmp/crdt_bench_detect.txt";
    write_file_from_lines(path, {"detect seed line"});
    atomic<long> seen_ns{0};
    atomic<bool> stop{false};
    thread w(bench_detect_watch, path, &seen_ns, &stop, use_kq);
    this_thread::sleep_for(chrono::milliseconds(use_kq ? 30 : 50));

    vector<double> samples;
    samples.reserve(trials);
    for (int i = 0; i < trials; i++)
    {
        long prev = seen_ns.load(memory_order_acquire);
        long t0 = monotonic_ns();
        write_file_from_lines(path, {"detect trial " + to_string(i) + " " + to_string(t0)});
        bool ok = wait_until([&] { return seen_ns.load(memory_order_acquire) != prev; },
                             use_kq ? 2000 : (POLL_INTERVAL_MS + 1500));
        long t1 = seen_ns.load(memory_order_acquire);
        if (ok && t1 >= t0)
            samples.push_back((double)(t1 - t0));
        else
            samples.push_back((double)(monotonic_ns() - t0));
    }
    stop.store(true);
    w.join();
    unlink(path.c_str());
    return summarize(samples);
}

Stats bench_ipc(bool blocking, int trials)
{
    string uid = blocking ? "ipcblk" : "ipcpoll";
    string p = pipe_name(uid);
    unlink(p.c_str());
    mkfifo(p.c_str(), 0666);

    g_running.store(true);
    g_quiet.store(true);
    g_headless.store(true);
    g_ipc_recv_count.store(0);
    g_apply_merges.store(false);
    recv_ptr = make_shared<UpdateVec>();
    local_ptr = make_shared<UpdateVec>();

    thread lis(listener_thread, uid, blocking);
    this_thread::sleep_for(chrono::milliseconds(50));

    int wr = open(p.c_str(), O_WRONLY);
    vector<double> samples;
    for (int i = 0; i < trials; i++)
    {
        UpdateObject upd = make_op(0, "a", "b" + to_string(i), uid);
        int before = g_ipc_recv_count.load(memory_order_acquire);
        long t0 = monotonic_ns();
        write_full(wr, &upd, sizeof(upd));
        wait_until([&] { return g_ipc_recv_count.load(memory_order_acquire) > before; }, 2000);
        long t1 = g_ipc_recv_ns.load(memory_order_acquire);
        if (t1 >= t0)
            samples.push_back((double)(t1 - t0));
    }
    close(wr);
    g_running.store(false);
    int dummy = g_listener_dummy_fd.exchange(-1);
    if (dummy != -1)
        close(dummy);
    int rd = g_listener_read_fd.exchange(-1);
    if (rd != -1)
        close(rd);
    lis.join();
    unlink(p.c_str());
    g_running.store(true);
    g_apply_merges.store(true);
    return summarize(samples);
}

struct MergeBench
{
    Stats stats;
    int merges = 0;
    double total_ms = 0;
};

MergeBench bench_merge(int threshold, int ops)
{
    string uid = "benchmerge";
    string filename = uid + "_doc.txt";
    vector<string> lines;
    for (int i = 0; i < 16; i++)
        lines.push_back("line " + to_string(i) + " original text for merge bench");
    write_file_from_lines(filename, lines);

    g_quiet.store(true);
    g_headless.store(true);
    recv_ptr = make_shared<UpdateVec>();
    local_ptr = make_shared<UpdateVec>();

    vector<double> samples;
    int merges = 0;
    vector<UpdateObject> batch;
    long t_all0 = monotonic_ns();
    for (int i = 0; i < ops; i++)
    {
        int line = i % 8;
        string oldc = "original";
        string newc = "v" + to_string(i);
        batch.push_back(make_op(line, oldc, newc, uid));
        if ((int)batch.size() >= threshold)
        {
            long t0 = monotonic_ns();
            merge_and_apply(batch, uid);
            samples.push_back((double)(monotonic_ns() - t0));
            batch.clear();
            recv_ptr = make_shared<UpdateVec>();
            merges++;
        }
    }
    long t_all1 = monotonic_ns();
    unlink(filename.c_str());
    MergeBench mb;
    mb.stats = summarize(samples);
    mb.merges = merges;
    mb.total_ms = (t_all1 - t_all0) / 1e6;
    return mb;
}

struct PayloadStats
{
    double span_bytes = 0;
    double line_bytes = 0;
    double file_bytes = 0;
    double vs_line_pct = 0;
    double vs_file_pct = 0;
    int n = 0;
};

PayloadStats bench_payload()
{
    vector<string> old_lines;
    old_lines.reserve(80);
    for (int i = 0; i < 80; i++)
        old_lines.push_back("Line " + to_string(i) +
                            " The quick brown fox jumps over the lazy dog; collaborative padding.");
    vector<string> new_lines = old_lines;
    new_lines[10] = "Line 10 The quick brown cat jumps over the lazy dog; collaborative padding.";
    new_lines[20] = "Line 20 The quick brown fox jumps over the lazy hog; collaborative padding.";
    new_lines[30] = "Line 30 The quick brown fox jumps over the lazy dog; collaborative session.";
    new_lines[40] = "Line 40 The quick brown fox jumps over the lazy dog; collaborative Padding.";

    int file_bytes = 0;
    for (auto &l : old_lines)
        file_bytes += (int)l.size() + 1;

    PayloadStats ps;
    ps.file_bytes = file_bytes;
    for (int i = 0; i < (int)old_lines.size(); i++)
    {
        int sc, ec;
        string op, np;
        if (!extract_replace_span(old_lines[i], new_lines[i], sc, ec, op, np))
            continue;
        ps.span_bytes += op.size() + np.size();
        ps.line_bytes += old_lines[i].size() + new_lines[i].size();
        ps.n++;
    }
    if (ps.n)
    {
        ps.span_bytes /= ps.n;
        ps.line_bytes /= ps.n;
        ps.vs_line_pct = 100.0 * (1.0 - ps.span_bytes / ps.line_bytes);
        if (ps.file_bytes > (double)sizeof(UpdateObject))
            ps.vs_file_pct = 100.0 * (1.0 - (double)sizeof(UpdateObject) / ps.file_bytes);
        else
            ps.vs_file_pct = 0;
    }
    return ps;
}

pid_t spawn_worker(const char *argv0, const char *user, bool use_kq)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        const char *watch = use_kq ? "--kqueue" : "--poll";
        execl(argv0, argv0, "--worker", user, watch, (char *)nullptr);
        perror("execl");
        _exit(127);
    }
    return pid;
}

bool file_contains(const string &path, const string &needle)
{
    ifstream f(path);
    string all((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    return all.find(needle) != string::npos;
}

Stats bench_converge(const char *argv0, bool use_kq, int trials)
{
    string reg = string("/crdtb") + to_string((long)getpid());
    setenv("CRDT_REGISTRY", reg.c_str(), 1);
    shm_unlink(reg.c_str());
    unlink("/tmp/pipe_benchA");
    unlink("/tmp/pipe_benchB");
    unlink("/tmp/crdt_ready_benchA");
    unlink("/tmp/crdt_ready_benchB");
    unlink("benchA_doc.txt");
    unlink("benchB_doc.txt");

    pid_t a = spawn_worker(argv0, "benchA", use_kq);
    bool ready_a = wait_until([] { return access("/tmp/crdt_ready_benchA", F_OK) == 0; }, 4000);
    pid_t b = spawn_worker(argv0, "benchB", use_kq);
    bool ready = ready_a && wait_until([]
                            { return access("/tmp/crdt_ready_benchB", F_OK) == 0 &&
                                     access("benchA_doc.txt", F_OK) == 0 &&
                                     access("benchB_doc.txt", F_OK) == 0; },
                            4000);
    vector<double> samples;
    if (!ready)
    {
        kill(a, SIGTERM);
        kill(b, SIGTERM);
        waitpid(a, nullptr, 0);
        waitpid(b, nullptr, 0);
        return summarize(samples);
    }

    vector<string> doc = {
        "Hello World",
        "This is a collaborative editor",
        "Welcome to SyncText",
        "Edit this document and see real-time updates",
        "Fifth line stays for merge threshold"};
    write_file_from_lines("benchA_doc.txt", doc);
    write_file_from_lines("benchB_doc.txt", doc);
    this_thread::sleep_for(chrono::milliseconds(use_kq ? 150 : POLL_INTERVAL_MS + 100));

    for (int t = 0; t < trials; t++)
    {
        string token = "TOK" + to_string(t) + "Z";
        doc[0] = "Hello " + token + "0";
        doc[1] = "This is " + token + "1";
        doc[2] = "Welcome " + token + "2";
        doc[3] = "Edit this " + token + "3";
        doc[4] = "Fifth " + token + "4";

        long t0 = monotonic_ns();
        write_file_from_lines("benchA_doc.txt", doc);
        bool ok = wait_until([&]
                             { return file_contains("benchB_doc.txt", token + "0") &&
                                      file_contains("benchB_doc.txt", token + "4"); },
                             use_kq ? 8000 : 12000);
        long t1 = monotonic_ns();
        if (ok)
            samples.push_back((double)(t1 - t0));
        this_thread::sleep_for(chrono::milliseconds(use_kq ? 30 : 50));
    }

    kill(a, SIGTERM);
    kill(b, SIGTERM);
    waitpid(a, nullptr, 0);
    waitpid(b, nullptr, 0);
    shm_unlink(reg.c_str());
    unlink("/tmp/pipe_benchA");
    unlink("/tmp/pipe_benchB");
    unlink("/tmp/crdt_ready_benchA");
    unlink("/tmp/crdt_ready_benchB");
    unlink("benchA_doc.txt");
    unlink("benchB_doc.txt");
    unsetenv("CRDT_REGISTRY");
    g_registry_shm = DEFAULT_REGISTRY_SHM;
    return summarize(samples);
}

int run_bench(const char *argv0)
{
    g_quiet.store(true);
    g_headless.store(true);
    cout << "CRDT microbenchmarks  (CLOCK_MONOTONIC, -O2 expected)\n";
    cout << "UpdateObject sizeof=" << sizeof(UpdateObject) << " bytes\n\n";

    ostringstream report;

#ifdef __APPLE__
    cout << "[1] File-watch detect latency (write -> watcher wakeup)\n";
    Stats det_kq = bench_detect(true, 80);
    print_stats("kqueue", det_kq);
    Stats det_poll = bench_detect(false, 8);
    print_stats("poll 2s", det_poll);
#else
    Stats det_kq{};
    Stats det_poll = bench_detect(false, 8);
    cout << "[1] File-watch detect latency\n";
    print_stats("poll 2s", det_poll);
#endif

    cout << "\n[2] FIFO IPC latency (write -> listener read complete)\n";
    Stats ipc_blk = bench_ipc(true, 200);
    print_stats("blocking + dummy writer", ipc_blk);
    Stats ipc_poll = bench_ipc(false, 80);
    print_stats("legacy 100ms poll", ipc_poll);

    cout << "\n[3] merge_and_apply wall time (headless, no terminal clear)\n";
    MergeBench m1 = bench_merge(1, 200);
    MergeBench m5 = bench_merge(5, 200);
    print_stats("threshold=1 (200 merges)", m1.stats);
    print_stats("threshold=5 (40 merges)", m5.stats);
    cout << "  total wall threshold=1: " << fixed << setprecision(2) << m1.total_ms << " ms, merges=" << m1.merges << "\n";
    cout << "  total wall threshold=5: " << m5.total_ms << " ms, merges=" << m5.merges << "\n";
    double merge_speedup = (m1.total_ms > 0) ? (m1.total_ms / m5.total_ms) : 0;
    cout << "  derived wall-clock speedup (1 vs 5): " << setprecision(2) << merge_speedup << "x\n";

    cout << "\n[4] Prefix/suffix payload vs full line / full file\n";
    PayloadStats ps = bench_payload();
    cout << "  mean span bytes (old+new): " << setprecision(1) << ps.span_bytes << "\n";
    cout << "  mean both-line bytes:      " << ps.line_bytes << "\n";
    cout << "  mean file bytes:           " << ps.file_bytes << "\n";
    cout << "  span vs line savings:      " << setprecision(1) << ps.vs_line_pct << "%\n";
    cout << "  608B op vs 80-line file:   " << ps.vs_file_pct << "% smaller IPC than sending the file\n";

    cout << "\n[5] End-to-end converge (2 processes, 5 non-overlapping line edits)\n";
#ifdef __APPLE__
    Stats conv_kq = bench_converge(argv0, true, 12);
    print_stats("kqueue converge", conv_kq);
#else
    Stats conv_kq{};
#endif
    Stats conv_poll = bench_converge(argv0, false, 4);
    print_stats("poll 2s converge", conv_poll);

    auto emit = [&](ostream &o)
    {
        o << "# Benchmark Results\n\n";
        o << "Generated by `./CRDT --bench` on this machine. All latencies use `CLOCK_MONOTONIC`.\n\n";
        o << "- Compiler: `g++ -std=c++17 -O2`\n";
        o << "- `sizeof(UpdateObject)` = " << sizeof(UpdateObject) << " bytes\n";
        o << "- Merge threshold in production = " << MERGE_THRESHOLD << "\n\n";
        o << "## 1. File-watch detect latency (file write → watcher wakeup)\n\n";
        o << "| Mode | n | mean | p50 | p99 | min | max |\n";
        o << "| --- | --- | --- | --- | --- | --- | --- |\n";
#ifdef __APPLE__
        o << "| kqueue vnode | " << det_kq.n << " | " << fmt_us(det_kq.mean_us) << " | " << fmt_us(det_kq.p50_us)
          << " | " << fmt_us(det_kq.p99_us) << " | " << fmt_us(det_kq.min_us) << " | " << fmt_us(det_kq.max_us) << " |\n";
#endif
        o << "| poll 2s `stat()` | " << det_poll.n << " | " << fmt_us(det_poll.mean_us) << " | " << fmt_us(det_poll.p50_us)
          << " | " << fmt_us(det_poll.p99_us) << " | " << fmt_us(det_poll.min_us) << " | " << fmt_us(det_poll.max_us) << " |\n\n";
        o << "## 2. FIFO IPC latency (write → listener `read` complete)\n\n";
        o << "| Mode | n | mean | p50 | p99 |\n";
        o << "| --- | --- | --- | --- | --- |\n";
        o << "| blocking FIFO + dummy writer | " << ipc_blk.n << " | " << fmt_us(ipc_blk.mean_us) << " | "
          << fmt_us(ipc_blk.p50_us) << " | " << fmt_us(ipc_blk.p99_us) << " |\n";
        o << "| legacy 100ms poll | " << ipc_poll.n << " | " << fmt_us(ipc_poll.mean_us) << " | "
          << fmt_us(ipc_poll.p50_us) << " | " << fmt_us(ipc_poll.p99_us) << " |\n\n";
        o << "## 3. Batched merge / file rewrite\n\n";
        o << "200 synthetic ops, headless (no `system(\"clear\")`).\n\n";
        o << "| Threshold | merges | total wall | per-merge p50 |\n";
        o << "| --- | --- | --- | --- |\n";
        o << "| 1 | " << m1.merges << " | " << fixed << setprecision(2) << m1.total_ms << " ms | " << fmt_us(m1.stats.p50_us) << " |\n";
        o << "| 5 | " << m5.merges << " | " << m5.total_ms << " ms | " << fmt_us(m5.stats.p50_us) << " |\n\n";
        o << "Derived wall-clock speedup (threshold 1 vs 5): **" << setprecision(2) << merge_speedup << "x**.\n";
        o << "Merge count reduction is exactly 200/40 = **5x** by construction.\n\n";
        o << "## 4. Diff payload\n\n";
        o << "Four 1-character (or short) edits on an **80-line** document. "
          << "IPC remains a fixed " << sizeof(UpdateObject) << "-byte POD.\n\n";
        o << "| Metric | Value |\n| --- | --- |\n";
        o << "| Mean changed-span bytes (old+new) | " << setprecision(1) << ps.span_bytes << " |\n";
        o << "| Mean both-line bytes | " << ps.line_bytes << " |\n";
        o << "| 80-line file bytes | " << ps.file_bytes << " |\n";
        o << "| Span vs full-line savings | " << ps.vs_line_pct << "% |\n";
        o << "| 608B op vs sending the 80-line file | " << ps.vs_file_pct << "% smaller |\n\n";
        o << "## 5. End-to-end converge (2 processes)\n\n";
        o << "One save of 5 non-overlapping line edits on A; wait until B's file contains all tokens.\n\n";
        o << "| Mode | n | mean | p50 | p99 |\n| --- | --- | --- | --- | --- |\n";
#ifdef __APPLE__
        o << "| kqueue | " << conv_kq.n << " | " << fmt_us(conv_kq.mean_us) << " | " << fmt_us(conv_kq.p50_us)
          << " | " << fmt_us(conv_kq.p99_us) << " |\n";
#endif
        o << "| poll 2s | " << conv_poll.n << " | " << fmt_us(conv_poll.mean_us) << " | " << fmt_us(conv_poll.p50_us)
          << " | " << fmt_us(conv_poll.p99_us) << " |\n\n";
        o << "## Notes\n\n";
        o << "- Detect/converge poll numbers include the **2s `stat()` sleep**; that is the legacy watch path, not merge CPU.\n";
        o << "- Converge `n` is successful trials only (timeouts are dropped, not counted as slow samples).\n";
        o << "- Span-vs-line savings are for short (≈1 character) edits on ~75-character lines.\n";
        o << "- LWW timestamps are `clock_gettime(CLOCK_REALTIME)` nanoseconds (not 1s `time()`).\n";
        o << "- Snapshot buffers use C++17 `atomic_load` / `atomic_compare_exchange` on `shared_ptr`.\n";
        o << "- Registry updates are serialized with `flock` (fixes the multi-process `user_count` race).\n";
        o << "- Re-run `./CRDT --bench` on another machine before citing these numbers.\n";
    };

    emit(cout);
    ofstream md("bench_results.md");
    emit(md);
    cout << "\nWrote bench_results.md\n";
    return 0;
}

// -------------------- Main --------------------
int main(int argc, char *argv[])
{
    if (const char *env = getenv("CRDT_REGISTRY"))
        g_registry_shm = env;

    if (argc >= 2 && string(argv[1]) == "--bench")
        return run_bench(argv[0]);

    bool worker = false;
    string user_id;
    for (int i = 1; i < argc; i++)
    {
        string a = argv[i];
        if (a == "--poll")
            g_use_kqueue.store(false);
        else if (a == "--kqueue")
            g_use_kqueue.store(true);
        else if (a == "--worker")
        {
            worker = true;
            g_headless.store(true);
            g_quiet.store(true);
        }
        else if (a == "--headless")
        {
            g_headless.store(true);
            g_quiet.store(true);
        }
        else if (a[0] != '-')
            user_id = a;
        else
        {
            cerr << "Unknown flag: " << a << "\n";
            return 1;
        }
    }

    if (user_id.empty())
    {
        cerr << "Usage:\n"
             << "  " << argv[0] << " <user_id>\n"
             << "  " << argv[0] << " --poll <user_id>\n"
             << "  " << argv[0] << " --bench\n";
        return 1;
    }

#ifdef __APPLE__
    (void)worker;
#else
    g_use_kqueue.store(false);
    (void)worker;
#endif

    run_editor(user_id);
    return 0;
}
