//
// utils.hpp
//
#ifndef __UTILS_HPP__
#define __UTILS_HPP__

#include <iostream>
#include <chrono>

namespace gen {

    // Lightweight utility class designed to measure the execution time of a code block.
    // It captures a high-resolution timestamp upon instantiation and automatically
    // calculates and logs the elapsed duration in milliseconds to the standard output
    // when it goes out of scope.
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
            //std::cout << prefix << duration.count() << " sec" << std::endl;

            // Convert to a fixed-point duration (e.g., milliseconds)
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
            std::cout << prefix << duration_ms.count() << " ms" << std::endl;
        }
    };

    // "Hex Dump" function: Convert raw binary data into a human-readable
    // hexadecimal string. It writes hex characters directly into a pre-allocated
    // char array.
    // NOTE: The 'dest' buffer must be at least (len * 2) in size.
    inline size_t ToHex(char* dest, const void* str, size_t len)
    {
        static const char* lut = "0123456789abcdef"; // Lookup table for hex digits

        // Tight loop using bit-shifting (fastest way to split a byte)
        const uint8_t* data = static_cast<const uint8_t*>(str);
        for(size_t i = 0; i < len; ++i)
        {
            *dest++ = lut[data[i] >> 4];   // High nibble
            *dest++ = lut[data[i] & 0x0f]; // Low nibble
        }

        return len * 2;
    }

    // "Hex Dump" function: Convert raw binary data into a human-readable
    // hexadecimal string
    inline std::string ToHex(const void* str, int len) 
    {
        if(len <= 0) 
            return {}; // Handle empty input gracefully
        std::string res;
        res.resize(len * 2);
        ToHex(&res[0], str, len);
        return res;
    }

    // "Hex Dump" function: Converts the entire contents of a string
    // into a human-readable hexadecimal string 
    inline std::string ToHex(const std::string& str) 
    { 
        return ToHex(str.data(), str.size()); 
    }

    // This constexpr method extracts the filename from a full path at compile time. 
    // It is intended for use with the __FILE__ macro and performs no argument validation.
    constexpr const char* GetFileName(const char* path) 
    {
        const char* lastSlash = path;
        for(const char* p = path; *p; ++p) 
        {
            if(*p == '/' || *p == '\\') 
                lastSlash = p + 1;
        }
        return lastSlash;
    }

    unsigned short ParseHostPort(const std::string& address, std::string& outHost, std::string& errMsg)
    {
        if(address.empty())
        {
            errMsg = "Input address is empty";
            return 0;
        }

        // Find the last colon (to correctly handle IPv6 and standard host:port)
        size_t lastColon = address.find_last_of(':');

        // Validation: Must have a colon, and it shouldn't be the last character
        if(lastColon == std::string::npos)
        {
            errMsg = "Invalid format. Expected host:port (missing ':')";
            return 0;
        }

        if(lastColon == address.length() - 1)
        {
            errMsg = "Invalid format. No port number after ':'";
            return 0;
        }

        // Split the string
        std::string hostPart = address.substr(0, lastColon);
        std::string portPart = address.substr(lastColon + 1);

        // Handle IPv6 bracket stripping: [::1] -> ::1
        if(!hostPart.empty() && hostPart.front() == '[' && hostPart.back() == ']')
        {
            errMsg = hostPart.substr(1, hostPart.length() - 2);
        }

        if(hostPart.empty())
        {
            errMsg = "Hostname part is empty";
            return 0;
        }

        try
        {
            int port = std::stoi(portPart);
            if(port < 1 || port > 65535)
            {
                throw std::out_of_range("Port out of range (1-65535)");
            }

            // Success: Assign host and return port
            outHost = hostPart;
            return static_cast<unsigned short>(port);
        }
        catch(const std::exception& e)
        {
            errMsg = "Error parsing port '" + portPart + "': " + e.what();
            return 0;
        }
    }

} // namespace gen

// Macro that strips the directory path from __FILE__.
// Performed at compile-time using the constexpr gen::GetFileName helper.
#define __FNAME__ gen::GetFileName(__FILE__)

#define FTRACE(msg) do { \
    std::cout << __FNAME__ << ":" << __LINE__ << " " << __func__ << "() " << msg << std::endl; \
} while(0)

#endif // __UTILS_HPP__
