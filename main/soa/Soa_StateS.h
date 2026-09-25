#ifndef SOA_STATES_H
#define SOA_STATES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOA_STATE_OK      0
#define SOA_STATE_NOT_OK  1

typedef uint8_t Soa_State_e;

typedef enum
{
    SOA_UNINITED,
    SOA_INITED,
    SOA_PHASE1,   //sending descriptor event and wait for get descriptor request from master ecu
    SOA_PHASE2,   //receive descriptor response, ready to work
    SOA_PHASE3,      //normal working
    /* @deprecated 旧的「主机准备休眠」标志, 原由 MIO 的协同休眠状态机
       (SleepMgr) 维护; 那套机制已从本工程移除。仅为兼容保留。 */
    SOA_READYSLEEP,
    SOA_ERROR,
}Soa_Mode;

typedef struct
{
    Soa_Mode mode;
}Soa_State_t;

Soa_State_e Soa_StateS_Init(Soa_State_t* state);
Soa_State_e Soa_StateS_SendDetection(Soa_State_t* state);
Soa_State_e Soa_StateS_GetDescriptorDone(Soa_State_t* state);
Soa_State_e Soa_StateS_ToWork(Soa_State_t* state);
void Soa_StateS_ToError(Soa_State_t* state);

#ifdef __cplusplus
}
#endif
#endif // SOA_STATES_H