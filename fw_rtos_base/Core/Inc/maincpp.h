#ifndef __MAINCPP_H
#define __MAINCPP_H
#include "FreeRTOS.h"

#ifdef __cplusplus
#include "main.h"
#include <new>
#include "cmsis_os.h"  
void *operator new(std::size_t size) throw(std::bad_alloc);
extern "C"
{
#endif

    void main_cpp(void);

#ifdef __cplusplus
}


#endif
#endif