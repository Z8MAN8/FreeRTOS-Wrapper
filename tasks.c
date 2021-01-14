/*
    Base on FreeRTOS V8.2.0
*/

/* Standard includes. */
#include <stdlib.h>
#include <string.h>

/* Defining MPU_WRAPPERS_INCLUDED_FROM_API_FILE prevents task.h from redefining
all the API functions to use the MPU wrappers.  That should only be done when
task.h is included from an application file. */
#define MPU_WRAPPERS_INCLUDED_FROM_API_FILE

/* FreeRTOS includes. */
#include "FreeRTOS.h"

#include <rthw.h>
#include <rtthread.h>
#include "task.h"
#include "timers.h"
#include "StackMacros.h"

#define _THREAD_TIMESLICE (10u)

/* Lint e961 and e750 are suppressed as a MISRA exception justified because the
MPU ports require MPU_WRAPPERS_INCLUDED_FROM_API_FILE to be defined for the
header files above, but not in this file, in order to generate the correct
privileged Vs unprivileged linkage and placement. */
#undef MPU_WRAPPERS_INCLUDED_FROM_API_FILE /*lint !e961 !e750. */

/* Set configUSE_STATS_FORMATTING_FUNCTIONS to 2 to include the stats formatting
functions but without including stdio.h here. */
#if ( configUSE_STATS_FORMATTING_FUNCTIONS == 1 )
    /* At the bottom of this file are two optional functions that can be used
    to generate human readable text from the raw data generated by the
    uxTaskGetSystemState() function.  Note the formatting functions are provided
    for convenience only, and are NOT considered part of the kernel. */
    #include <stdio.h>
#endif /* configUSE_STATS_FORMATTING_FUNCTIONS == 1 ) */

/* Sanity check the configuration. */
#if configUSE_TICKLESS_IDLE != 0
    #if INCLUDE_vTaskSuspend != 1
        #error INCLUDE_vTaskSuspend must be set to 1 if configUSE_TICKLESS_IDLE is not set to 0
    #endif /* INCLUDE_vTaskSuspend */
#endif /* configUSE_TICKLESS_IDLE */

#define MT2625_TASK_DEBUG

#ifdef MT2625_TASK_DEBUG
#define LOG_TAG              "TASK"
#define LOG_LVL              LOG_LVL_ERROR// LOG_LVL_ERROR | LOG_LVL_DBG
#include <ulog.h>
#else
#define LOG_I(...)
#endif

#define RT_REVERT_PRIORITY   64

/* Value that can be assigned to the eNotifyState member of the TCB. */
typedef enum
{
    eNotWaitingNotification = 0,
    eWaitingNotification,
    eNotified
} eNotifyValue;

/*
 * Some kernel aware debuggers require the data the debugger needs access to to
 * be global, rather than file scope.
 */
#ifdef portREMOVE_STATIC_QUALIFIER
    #define static
#endif

/*lint -e956 A manual analysis and inspection has been used to determine which
static variables must be declared volatile. */

// PRIVILEGED_DATA TCB_t * volatile pxCurrentTCB = NULL;

/* Other file private variables. --------------------------------*/
PRIVILEGED_DATA static volatile BaseType_t xSchedulerRunning        = pdFALSE;
#define RTT_USING_CPUID 0
#define portNUM_PROCESSORS 20

PRIVILEGED_DATA static volatile UBaseType_t uxCurrentNumberOfTasks  = ( UBaseType_t ) 0U;
portSTACK_TYPE * volatile pxCurrentTCB[ portNUM_PROCESSORS ] = { NULL };
portSTACK_TYPE * volatile pxSaveTCB[ portNUM_PROCESSORS ] = { NULL };

/* File private functions. --------------------------------*/

BaseType_t xTaskGenericCreate( TaskFunction_t pxTaskCode, const char * const pcName, 
    const uint16_t usStackDepth, void * const pvParameters, UBaseType_t uxPriority, 
    TaskHandle_t * const pxCreatedTask, StackType_t * const puxStackBuffer, const MemoryRegion_t * const xRegions ) /*lint !e971 Unqualified char types are allowed for strings and single characters only. */
{
    BaseType_t xReturn = pdFAIL;
    rt_thread_t tid = RT_NULL;
    uint32_t usStackDepthC;
    int rt_priority = RT_REVERT_PRIORITY - uxPriority;

    configASSERT( pxTaskCode );

    usStackDepthC = usStackDepth*sizeof(StackType_t) + 1024U + 512u;

    LOG_D("task create - name:%s; stack size:%d; priority:%d; raw:%d", pcName, usStackDepthC, rt_priority, uxPriority);
    tid = rt_thread_create((const char *)pcName, pxTaskCode, pvParameters,
                (rt_uint32_t)usStackDepthC, (rt_uint8_t)rt_priority, _THREAD_TIMESLICE);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
        xReturn = pdPASS;
        uxCurrentNumberOfTasks ++;
    }
    else
    {
        LOG_E("f:%s;l:%d. thread create failed. task name:%s", __FUNCTION__, __LINE__, pcName);
    }

    if (pxCreatedTask != RT_NULL)
    {
        *pxCreatedTask = (TaskHandle_t *)tid;
    }

    return xReturn;
}
/*-----------------------------------------------------------*/

#if ( INCLUDE_vTaskDelete == 1 )

    void vTaskDelete( TaskHandle_t xTaskToDelete )
    {
        rt_thread_t thread = (rt_thread_t)xTaskToDelete;

        if (thread == NULL) thread = rt_thread_self();
        LOG_D("f:%s;l:%d. rt_thread_delete.", __FUNCTION__, __LINE__);
        rt_thread_delete(thread);
        rt_schedule();
    }

#endif /* INCLUDE_vTaskDelete */
/*-----------------------------------------------------------*/

#if ( INCLUDE_vTaskDelayUntil == 1 )

    void vTaskDelayUntil( TickType_t * const pxPreviousWakeTime, const TickType_t xTimeIncrement )
    {
        rt_tick_t tick = rt_tick_get();
        rt_tick_t tick_to_delay = *pxPreviousWakeTime + xTimeIncrement;

        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);
        if ((tick_to_delay - tick) < RT_TICK_MAX / 2)
        {
            tick = tick_to_delay - tick;

            rt_thread_delay(tick);
        }

        *pxPreviousWakeTime = rt_tick_get();
    }

#endif /* INCLUDE_vTaskDelayUntil */
/*-----------------------------------------------------------*/

#if ( INCLUDE_vTaskDelay == 1 )

    void vTaskDelay( const TickType_t xTicksToDelay )
    {
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);
        rt_thread_delay((rt_tick_t)xTicksToDelay);
    }

#endif /* INCLUDE_vTaskDelay */
/*-----------------------------------------------------------*/

#if ( INCLUDE_eTaskGetState == 1 )

    eTaskState eTaskGetState( TaskHandle_t xTask )
    {
        eTaskState eReturn;
        rt_uint8_t stat;
        rt_thread_t thread = (rt_thread_t)xTask;

        if (thread == NULL)
            thread = rt_thread_self();
        if (thread == NULL)
        {
            eReturn = eDeleted;
            LOG_E("f:%s;l:%d.[ERROR]", __FUNCTION__, __LINE__);
            RT_ASSERT(0);
        }

        // rt_enter_critical();
        stat = thread->stat & RT_THREAD_STAT_MASK;
        // rt_exit_critical();

        LOG_D("f:%s;l:%d. T:%s; State:%d", __FUNCTION__, __LINE__, thread->name, stat);

        switch(stat)
        {
            case RT_THREAD_RUNNING:
                eReturn = eRunning;
                break;
            case RT_THREAD_READY:
                eReturn = eReady;
                break;
            case RT_THREAD_SUSPEND:
                eReturn = eSuspended;
                break;
            case RT_THREAD_CLOSE:
                eReturn = eDeleted;
                break;
            default:
                eReturn = 7; /* unknown stat */
                LOG_E("error task status!");
                break;
        }

        return eReturn;
    } /*lint !e818 xTask cannot be a pointer to const because it is a typedef. */

#endif /* INCLUDE_eTaskGetState */
/*-----------------------------------------------------------*/

#if ( INCLUDE_uxTaskPriorityGet == 1 )

    UBaseType_t uxTaskPriorityGet( TaskHandle_t xTask )
    {
        rt_uint8_t perio;
        rt_thread_t thread = (rt_thread_t)xTask;

        if (thread == NULL) thread = rt_thread_self();
        RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

        perio = RT_REVERT_PRIORITY - thread->init_priority;

        LOG_D("f:%s;l:%d.pri:%d; thread:%s", __FUNCTION__, __LINE__, perio, thread->name);
        return (UBaseType_t)perio;
    }

#endif /* INCLUDE_uxTaskPriorityGet */
/*-----------------------------------------------------------*/

#if ( INCLUDE_uxTaskPriorityGet == 1 )

    UBaseType_t uxTaskPriorityGetFromISR( TaskHandle_t xTask )
    {
        rt_uint8_t perio;

        rt_thread_t thread = (rt_thread_t)xTask;

        if (thread == NULL) thread = rt_thread_self();
        RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

        perio = RT_REVERT_PRIORITY - thread->init_priority;
        LOG_D("f:%s;l:%d.pri:%d", __FUNCTION__, __LINE__, perio);

        return (UBaseType_t)perio;
    }

#endif /* INCLUDE_uxTaskPriorityGet */
/*-----------------------------------------------------------*/

#if ( INCLUDE_vTaskPrioritySet == 1 )

    void vTaskPrioritySet( TaskHandle_t xTask, UBaseType_t uxNewPriority )
    {
        int rt_priority = RT_REVERT_PRIORITY - uxNewPriority;
        rt_thread_t thread = (rt_thread_t)xTask;

        RT_ASSERT(uxNewPriority < configMAX_PRIORITIES);

        if (thread == NULL) thread = rt_thread_self();
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);

        LOG_D("set priority -> %d", rt_priority);
        rt_thread_control(thread, RT_THREAD_CTRL_CHANGE_PRIORITY, &rt_priority);
    }

#endif /* INCLUDE_vTaskPrioritySet */
/*-----------------------------------------------------------*/

#if ( INCLUDE_vTaskSuspend == 1 )

    void vTaskSuspend( TaskHandle_t xTaskToSuspend )
    {
        rt_thread_t thread = (rt_thread_t)xTaskToSuspend;
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);

        if (thread == NULL)
            thread = rt_thread_self();
        rt_thread_suspend(thread);

        rt_schedule();
    }

#endif /* INCLUDE_vTaskSuspend */
/*-----------------------------------------------------------*/

#if ( INCLUDE_vTaskSuspend == 1 )

    void vTaskResume( TaskHandle_t xTaskToResume )
    {
        rt_thread_t thread = (rt_thread_t)xTaskToResume;

        RT_ASSERT(thread != RT_NULL);
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);

        rt_thread_resume(thread);
        rt_schedule();
    }

#endif /* INCLUDE_vTaskSuspend */

/*-----------------------------------------------------------*/

#if ( ( INCLUDE_xTaskResumeFromISR == 1 ) && ( INCLUDE_vTaskSuspend == 1 ) )

    BaseType_t xTaskResumeFromISR( TaskHandle_t xTaskToResume )
    {
        rt_thread_t thread = (rt_thread_t)xTaskToResume;

        RT_ASSERT(thread != RT_NULL);
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);

        rt_thread_resume(thread);
        rt_schedule();

        return pdTRUE;
    }

#endif /* ( ( INCLUDE_xTaskResumeFromISR == 1 ) && ( INCLUDE_vTaskSuspend == 1 ) ) */
/*-----------------------------------------------------------*/

void vTaskStartScheduler( void )
{
    xSchedulerRunning=pdTRUE;
    // LOG_I("f:%s;l:%d.Will start scheduler.", __FUNCTION__, __LINE__);
    // xPortStartScheduler();
    // rt_system_scheduler_start();
}
/*-----------------------------------------------------------*/

void vTaskSuspendAll( void )
{
    LOG_D("f:%s;l:%d.vTaskSuspendAll.", __FUNCTION__, __LINE__);
    if( xSchedulerRunning != pdFALSE )
        rt_enter_critical();
    else
    {
        LOG_E("f:%s;l:%d.[ERROR].", __FUNCTION__, __LINE__);
    }
}
/*----------------------------------------------------------*/

BaseType_t xTaskResumeAll( void )
{
    LOG_D("f:%s;l:%d.xTaskResumeAll.", __FUNCTION__, __LINE__);
    if( xSchedulerRunning != pdFALSE )
        rt_exit_critical();
    else
    {
        LOG_E("f:%s;l:%d.[ERROR].", __FUNCTION__, __LINE__);
    }
    return pdTRUE;
}
/*-----------------------------------------------------------*/

TickType_t xTaskGetTickCount( void )
{
    LOG_D("f:%s;l:%d.xTaskGetTickCount.", __FUNCTION__, __LINE__);
    return (TickType_t)rt_tick_get();
}

TickType_t xTaskGetTickCountFromISR( void )
{
    LOG_D("f:%s;l:%d.xTaskGetTickCountFromISR.", __FUNCTION__, __LINE__);
    return (TickType_t)rt_tick_get();
}

/*-----------------------------------------------------------*/

UBaseType_t uxTaskGetNumberOfTasks( void )
{
    struct rt_object_information *information;
    UBaseType_t thread_num = 0;

    LOG_D("f:%s;l:%d.get_thread_num.", __FUNCTION__, __LINE__);

    /* enter critical */
    rt_enter_critical();

    information = rt_object_get_information(RT_Object_Class_Thread);
    if (information)
    {
        struct rt_list_node *node = RT_NULL;

        for (node  = information->object_list.next;
                node != &(information->object_list);
                node  = node->next)
        {
            thread_num ++;
        }
    }

    /* leave critical */
    rt_exit_critical();

    LOG_D("f:%s;l:%d.thread_num:%d:%d", __FUNCTION__, __LINE__, thread_num, uxCurrentNumberOfTasks);

    return thread_num;
}
/*-----------------------------------------------------------*/

#if ( INCLUDE_pcTaskGetTaskName == 1 )

    char *pcTaskGetTaskName( TaskHandle_t xTaskToQuery ) /*lint !e971 Unqualified char types are allowed for strings and single characters only. */
    {
        rt_thread_t thread = (rt_thread_t)(xTaskToQuery);

        if (thread == NULL)
            thread = rt_thread_self();
        if (thread == NULL)
        {
            LOG_E("f:%s;l:%d.[ERROR]", __FUNCTION__, __LINE__);
            RT_ASSERT(0);
        }

        return thread->name;;
    }

#endif /* INCLUDE_pcTaskGetTaskName */
/*-----------------------------------------------------------*/

/* This conditional compilation should use inequality to 0, not equality to 1.
This is to ensure vTaskStepTick() is available when user defined low power mode
implementations require configUSE_TICKLESS_IDLE to be set to a value other than
1. */
#if ( configUSE_TICKLESS_IDLE != 0 )

    /* RT-Thread TODO */
    void vTaskStepTick( const TickType_t xTicksToJump )
    {
        /* Correct the tick count value after a period during which the tick
        was suppressed.  Note this does *not* call the tick hook function for
        each stepped tick. */
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);
        rt_tick_set(rt_tick_get() + xTicksToJump);
    }

#endif /* configUSE_TICKLESS_IDLE */
/*----------------------------------------------------------*/

/* RT-Thread TODO */
BaseType_t xTaskIncrementTick( void )
{
    LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);
    rt_tick_increase();
    return pdTRUE;
}
/*-----------------------------------------------------------*/

void vTaskSwitchContext( void )
{
    LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);
    rt_schedule();
}
/*-----------------------------------------------------------*/

#if configUSE_TICKLESS_IDLE != 0

    eSleepModeStatus eTaskConfirmSleepModeStatus( void )
    {
        /* RT-Thread TODO*/
        return eAbortSleep;
    }
#endif /* configUSE_TICKLESS_IDLE */
/*-----------------------------------------------------------*/

#if ( INCLUDE_uxTaskGetStackHighWaterMark == 1 )

    UBaseType_t uxTaskGetStackHighWaterMark( TaskHandle_t xTask )
    {
        rt_ubase_t free_space = 0;
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);

        rt_thread_t thread = (rt_thread_t)xTask;
        if (thread == NULL)
            thread = rt_thread_self();
        if (thread == NULL)
        {
            LOG_E("f:%s;l:%d.", __FUNCTION__, __LINE__);
            RT_ASSERT(0);
        }

        {
            rt_uint8_t *ptr;

#if defined(ARCH_CPU_STACK_GROWS_UPWARD)
            ptr = (rt_uint8_t *)thread->stack_addr + thread->stack_size - 1;
            while (*ptr == '#')ptr --;

            free_space = thread->stack_size - (rt_ubase_t)ptr + (rt_ubase_t)thread->stack_addr;
#else
            ptr = (rt_uint8_t *)thread->stack_addr;
            while (*ptr == '#')ptr ++;

            free_space = (rt_ubase_t) ptr + (rt_ubase_t) thread->stack_addr;
            goto __continue;
#endif
        }

__continue:
        free_space /= ( uint32_t ) sizeof( StackType_t );
        LOG_D("f:%s;l:%d; free_space:%d", __FUNCTION__, __LINE__, free_space);

        return free_space;
    }

#endif /* INCLUDE_uxTaskGetStackHighWaterMark */
/*-----------------------------------------------------------*/

#if ( ( INCLUDE_xTaskGetCurrentTaskHandle == 1 ) || ( configUSE_MUTEXES == 1 ) )

    TaskHandle_t xTaskGetCurrentTaskHandle( void )
    {
        // LOG_I("f:%s;l:%d.", __FUNCTION__, __LINE__);
        // LOG_I("xTaskGetCurrentTaskHandle, current Task: %*.s", RT_NAME_MAX, rt_thread_self()->name);

        return rt_thread_self();
    }

#endif /* ( ( INCLUDE_xTaskGetCurrentTaskHandle == 1 ) || ( configUSE_MUTEXES == 1 ) ) */
/*-----------------------------------------------------------*/

#if ( ( INCLUDE_xTaskGetSchedulerState == 1 ) || ( configUSE_TIMERS == 1 ) )
    /* TODO: temporary placed here */
    #include <stdbool.h>
    bool is_in_exception(void)
    {
        extern bool IsSystemEnterException;

        return IsSystemEnterException;
    }

    BaseType_t xTaskGetSchedulerState( void )
    {
        BaseType_t xReturn = taskSCHEDULER_NOT_STARTED;
        extern bool is_in_exception(void);

        if (is_in_exception() == true)
        {
            return xReturn;
        }

        if( xSchedulerRunning != pdFALSE )
        {
            if (rt_critical_level() == 0)
            {
                xReturn = taskSCHEDULER_RUNNING;
            }
            else
            {
                xReturn = taskSCHEDULER_SUSPENDED;
            }
        }

        return xReturn;
    }

#endif /* ( ( INCLUDE_xTaskGetSchedulerState == 1 ) || ( configUSE_TIMERS == 1 ) ) */
/*-----------------------------------------------------------*/

#if ( portCRITICAL_NESTING_IN_TCB == 1 )

    void vTaskEnterCritical( void ) /* Not run */
    {
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);
        vPortCPUAcquireMutex( mux );
        if( xSchedulerRunning != pdFALSE )
            rt_enter_critical();
    }

#endif /* portCRITICAL_NESTING_IN_TCB */
/*-----------------------------------------------------------*/

#if ( portCRITICAL_NESTING_IN_TCB == 1 )

    void vTaskExitCritical( void )
    {
        LOG_D("f:%s;l:%d.", __FUNCTION__, __LINE__);
        vPortCPUReleaseMutex( mux );
        if( xSchedulerRunning != pdFALSE )
            rt_exit_critical();
    }

#endif /* portCRITICAL_NESTING_IN_TCB */
/*-----------------------------------------------------------*/

#if ( ( configUSE_TRACE_FACILITY == 1 ) && ( configUSE_STATS_FORMATTING_FUNCTIONS > 0 ) )

    void vTaskList( char * pcWriteBuffer )
    {
        /* RT-Thread TODO */
        #warning "TODO"
        // int32_t buf_size = uxTaskGetNumberOfTasks() * (configMAX_TASK_NAME_LEN + 18);
        LOG_E("f:%s;l:%d.", __FUNCTION__, __LINE__);

        /* Make sure the write buffer does not contain a string. */
        *pcWriteBuffer = 0x00;

        // rt_strncpy(pcWriteBuffer, "RT-Thread TODO", buf_size);
        // pcWriteBuffer[buf_size - 1] = 0x00;
    }

#endif /* ( ( configUSE_TRACE_FACILITY == 1 ) && ( configUSE_STATS_FORMATTING_FUNCTIONS > 0 ) ) */
/*----------------------------------------------------------*/

#if ( ( configGENERATE_RUN_TIME_STATS == 1 ) && ( configUSE_STATS_FORMATTING_FUNCTIONS > 0 ) )

    void vTaskGetRunTimeStats( char *pcWriteBuffer )
    {
        /* RT-Thread TODO */
        #warning "TODO"
        // int32_t buf_size = uxTaskGetNumberOfTasks() * (configMAX_TASK_NAME_LEN + 20);
        LOG_E("f:%s;l:%d.", __FUNCTION__, __LINE__);

        /* Make sure the write buffer does not contain a string. */
        *pcWriteBuffer = 0x00;

        // rt_strncpy(pcWriteBuffer, "RT-Thread TODO", buf_size);
        // pcWriteBuffer[buf_size - 1] = 0x00;
    }

#endif /* ( ( configGENERATE_RUN_TIME_STATS == 1 ) && ( configUSE_STATS_FORMATTING_FUNCTIONS > 0 ) ) */
/*-----------------------------------------------------------*/

#if ( configGENERATE_RUN_TIME_STATS == 1 )

    void vTaskClearTaskRunTimeCounter( void )
    {
        #warning "TODO"

        /* RT-Thread TODO */
        LOG_E("f:%s;l:%d.", __FUNCTION__, __LINE__);
    }

#endif /* configGENERATE_RUN_TIME_STATS */

UBaseType_t uxTaskGetBottomOfStack(TaskHandle_t xTaskHandle)
{
    rt_ubase_t stack_end_p = 0;

    rt_thread_t thread = (rt_thread_t)xTaskHandle;
    if (thread == NULL) thread = rt_thread_self();

#if defined(ARCH_CPU_STACK_GROWS_UPWARD)
    stack_end_p = (rt_ubase_t)((rt_uint8_t *)thread->stack_addr + thread->stack_size - 1);
#else
    stack_end_p = (rt_ubase_t)(thread->stack_addr);
#endif

    LOG_D("f:%s;l:%d; stack_end_p:%p", __FUNCTION__, __LINE__, stack_end_p);

    return stack_end_p;
}

void *uxTaskGetEventListItemContainer(TaskHandle_t xTaskHandle) /* TODO */
{
#if 0
    TCB_t *pxTCB;

    pxTCB = ( xTaskHandle == NULL )? ( TCB_t * ) pxCurrentTCB : ( TCB_t * ) ( xTaskHandle );

    return ( pxTCB->xEventListItem.pvContainer );
#else
    // rt_thread_t thread;
    LOG_E("f:%s;l:%d.", __FUNCTION__, __LINE__);
    return NULL;
#endif
}
/*-----------------------------------------------------------*/
