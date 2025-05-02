//
// logger.hpp
//
#ifndef __LOGGER_HPP__
#define __LOGGER_HPP__

//
// Thread-safe logging
//
#include <iostream>         // cout
#include <mutex>            // mutex, unique_lock
#include <unistd.h>         // syscall()
#include <sys/syscall.h>    // __NR_gettid

namespace logger
{
// Mutex to sync multi-threading logging
inline std::mutex& GetLogMutex()
{
    static std::mutex sLogMutex;
    return sLogMutex;
}

inline pid_t GetThreadId()
{
    static thread_local pid_t threadId = syscall(__NR_gettid);
    return threadId;
}

//#ifndef __FNAME__
//    // This constexpr method extracts the filename from a full path using
//    // the __FILE__ preprocessor variable, resolving at compile time.
//    constexpr const char* fname(const char* file, int i)
//    {
//        return (i == 0) ? (file) : (*(file + i) == '/' ? (file + i + 1) : fname(file, i - 1));
//    }
//    #define __FNAME__ logger::fname(__FILE__, sizeof(__FILE__)-1)
//#endif
} // end of namespace logger


#define __MSG__(msg_type, msg)                                  \
do{                                                             \
    std::unique_lock<std::mutex> lock(logger::GetLogMutex());   \
    std::cout << "[" << logger::GetThreadId() << "]"            \
              << (*msg_type == '\0' ? "" : "[" msg_type "]")    \
              << " " << __func__ << ": " << msg << std::endl;   \
}while(0)

#define OUTMSG(msg)    __MSG__("", msg)
#define INFOMSG(msg)   __MSG__("INFO", msg)
#define ERRORMSG(msg)  __MSG__("ERROR", msg)

//
// Helper StopWatch class to measure elapsed time
//
#include <string>   // std::string
#include <chrono>   // std::chrono

class StopWatch
{
    std::chrono::time_point<std::chrono::high_resolution_clock> start;
    std::chrono::time_point<std::chrono::high_resolution_clock> stop;
    std::string prefix;

public:
    StopWatch(const char* _prefix="") : prefix(_prefix)
    {
        start = std::chrono::high_resolution_clock::now();
    }
    ~StopWatch()
    {
        stop = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> duration = stop - start;
        std::cout << prefix << duration.count() << " sec" << std::endl;
    }
};

#endif // __LOGGER_HPP__

