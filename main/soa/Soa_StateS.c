#include "Soa_StateS.h"

Soa_State_e Soa_StateS_Init(Soa_State_t* state)
{
    if (state != NULL)
    {
        state->mode = SOA_INITED;
        return SOA_STATE_OK;
    }
    return SOA_STATE_NOT_OK;
}

Soa_State_e Soa_StateS_SendDetection(Soa_State_t* state)
{
    if ((state != NULL) && (state->mode == SOA_INITED))
    {
        state->mode = SOA_PHASE1;
        return SOA_STATE_OK;
    }
    return SOA_STATE_NOT_OK;
}


Soa_State_e Soa_StateS_GetDescriptorDone(Soa_State_t* state)
{
    if ((state != NULL) && (state->mode == SOA_PHASE1))
    {
        state->mode = SOA_PHASE2;
        return SOA_STATE_OK;
    }
    return SOA_STATE_NOT_OK;
}

Soa_State_e Soa_StateS_ToWork(Soa_State_t* state)
{
    if ((state != NULL) && (state->mode == SOA_PHASE2))
    {
        state->mode = SOA_PHASE3;
        return SOA_STATE_OK;
    }
    return SOA_STATE_NOT_OK;
}

void Soa_StateS_ToError(Soa_State_t* state)
{
    if ((state != NULL))
    {
        state->mode = SOA_ERROR;
    }
}
