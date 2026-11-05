#pragma once

#if defined(_WIN32)
  #if defined(RVDL_RT_BUILD)
    #define RVDL_RT_API __declspec(dllexport)
  #else
    #define RVDL_RT_API __declspec(dllimport)
  #endif
#else
  #define RVDL_RT_API __attribute__((visibility("default")))
#endif
