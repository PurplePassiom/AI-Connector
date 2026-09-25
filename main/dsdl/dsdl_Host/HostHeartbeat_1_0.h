// This is an AUTO-GENERATED Cyphal DSDL data type implementation. Curious? See https://opencyphal.org.
// You shouldn't attempt to edit this file.
//
// Checking this file under version control is not recommended unless it is used as part of a high-SIL
// safety-critical codebase. The typical usage scenario is to generate it as part of the build process.
//
// To avoid conflicts with definitions given in the source DSDL file, all entities created by the code generator
// are named with an underscore at the end, like foo_bar_().
//
// Generator:     nunavut-2.3.1 (serialization was enabled)
// Source file:   D:/Iyou/08_DSDL/dsdl_Host/6152.HostHeartbeat.1.0.dsdl
// Generated at:  2026-09-20 04:45:54.446835 UTC
// Is deprecated: no
// Fixed port-ID: 6152
// Full name:     dsdl_Host.HostHeartbeat
// Version:       1.0
//
// Platform
//     python_implementation:  CPython
//     python_version:  3.14.6
//     python_release_level:  final
//     python_build:  ('tags/v3.14.6:c63aec6', 'Jun 10 2026 10:26:10')
//     python_compiler:  MSC v.1944 64 bit (AMD64)
//     python_revision:  c63aec6
//     python_xoptions:  {}
//     runtime_platform:  Windows-11-10.0.26200-SP0
//
// Language Options
//     target_endianness:  any
//     omit_float_serialization_support:  False
//     enable_serialization_asserts:  False
//     enable_override_variable_array_capacity:  False
//     cast_format:  (({type}) {value})

#ifndef DSDL_HOST_HOST_HEARTBEAT_1_0_INCLUDED_
#define DSDL_HOST_HOST_HEARTBEAT_1_0_INCLUDED_

#include <nunavut/support/serialization.h>
#include <stdint.h>
#include <stdlib.h>

static_assert( NUNAVUT_SUPPORT_LANGUAGE_OPTION_TARGET_ENDIANNESS == 1693710260,
              "D:/Iyou/08_DSDL/dsdl_Host/6152.HostHeartbeat.1.0.dsdl is trying to use a serialization library that was compiled with "
              "different language options. This is dangerous and therefore not allowed." );
static_assert( NUNAVUT_SUPPORT_LANGUAGE_OPTION_OMIT_FLOAT_SERIALIZATION_SUPPORT == 0,
              "D:/Iyou/08_DSDL/dsdl_Host/6152.HostHeartbeat.1.0.dsdl is trying to use a serialization library that was compiled with "
              "different language options. This is dangerous and therefore not allowed." );
static_assert( NUNAVUT_SUPPORT_LANGUAGE_OPTION_ENABLE_SERIALIZATION_ASSERTS == 0,
              "D:/Iyou/08_DSDL/dsdl_Host/6152.HostHeartbeat.1.0.dsdl is trying to use a serialization library that was compiled with "
              "different language options. This is dangerous and therefore not allowed." );
static_assert( NUNAVUT_SUPPORT_LANGUAGE_OPTION_ENABLE_OVERRIDE_VARIABLE_ARRAY_CAPACITY == 0,
              "D:/Iyou/08_DSDL/dsdl_Host/6152.HostHeartbeat.1.0.dsdl is trying to use a serialization library that was compiled with "
              "different language options. This is dangerous and therefore not allowed." );
static_assert( NUNAVUT_SUPPORT_LANGUAGE_OPTION_CAST_FORMAT == 2368206204,
              "D:/Iyou/08_DSDL/dsdl_Host/6152.HostHeartbeat.1.0.dsdl is trying to use a serialization library that was compiled with "
              "different language options. This is dangerous and therefore not allowed." );

#ifdef __cplusplus
extern "C" {
#endif

#define dsdl_Host_HostHeartbeat_1_0_HAS_FIXED_PORT_ID_ true
#define dsdl_Host_HostHeartbeat_1_0_FIXED_PORT_ID_     6152U

// +-------------------------------------------------------------------------------------------------------------------+
// | dsdl_Host.HostHeartbeat.1.0
// +-------------------------------------------------------------------------------------------------------------------+
#define dsdl_Host_HostHeartbeat_1_0_FULL_NAME_             "dsdl_Host.HostHeartbeat"
#define dsdl_Host_HostHeartbeat_1_0_FULL_NAME_AND_VERSION_ "dsdl_Host.HostHeartbeat.1.0"

/// Extent is the minimum amount of memory required to hold any serialized representation of any compatible
/// version of the data type; or, on other words, it is the the maximum possible size of received objects of this type.
/// The size is specified in bytes (rather than bits) because by definition, extent is an integer number of bytes long.
/// When allocating a deserialization (RX) buffer for this data type, it should be at least extent bytes large.
/// When allocating a serialization (TX) buffer, it is safe to use the size of the largest serialized representation
/// instead of the extent because it provides a tighter bound of the object size; it is safe because the concrete type
/// is always known during serialization (unlike deserialization). If not sure, use extent everywhere.
#define dsdl_Host_HostHeartbeat_1_0_EXTENT_BYTES_                    7UL
#define dsdl_Host_HostHeartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_ 5UL
static_assert(dsdl_Host_HostHeartbeat_1_0_EXTENT_BYTES_ >= dsdl_Host_HostHeartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_,
              "Internal constraint violation");

/// saturated uint4 HOST_MODE_UNINITIALIZED = 0
#define dsdl_Host_HostHeartbeat_1_0_HOST_MODE_UNINITIALIZED (0U)

/// saturated uint4 HOST_MODE_DISCOVERY = 1
#define dsdl_Host_HostHeartbeat_1_0_HOST_MODE_DISCOVERY (1U)

/// saturated uint4 HOST_MODE_INITIALIZING = 2
#define dsdl_Host_HostHeartbeat_1_0_HOST_MODE_INITIALIZING (2U)

/// saturated uint4 HOST_MODE_WORKING = 3
#define dsdl_Host_HostHeartbeat_1_0_HOST_MODE_WORKING (3U)

/// saturated uint4 HOST_MODE_WAKING = 4
#define dsdl_Host_HostHeartbeat_1_0_HOST_MODE_WAKING (4U)

/// saturated uint4 HOST_MODE_SLEEP_PREPARING = 6
#define dsdl_Host_HostHeartbeat_1_0_HOST_MODE_SLEEP_PREPARING (6U)

/// saturated uint4 HOST_MODE_SLEEPING = 8
#define dsdl_Host_HostHeartbeat_1_0_HOST_MODE_SLEEPING (8U)

/// saturated uint2 HOST_HEALTH_NOMINAL = 0
#define dsdl_Host_HostHeartbeat_1_0_HOST_HEALTH_NOMINAL (0U)

/// saturated uint2 HOST_HEALTH_ADVISORY = 1
#define dsdl_Host_HostHeartbeat_1_0_HOST_HEALTH_ADVISORY (1U)

/// saturated uint2 HOST_HEALTH_CAUTION = 2
#define dsdl_Host_HostHeartbeat_1_0_HOST_HEALTH_CAUTION (2U)

/// saturated uint2 HOST_HEALTH_WARNING = 3
#define dsdl_Host_HostHeartbeat_1_0_HOST_HEALTH_WARNING (3U)

/// saturated uint5 HOST_SLEEP_REASON_NONE = 0
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_NONE (0U)

/// saturated uint5 HOST_SLEEP_REASON_ACC_OFF = 1
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_ACC_OFF (1U)

/// saturated uint5 HOST_SLEEP_REASON_KEY_OFF = 2
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_KEY_OFF (2U)

/// saturated uint5 HOST_SLEEP_REASON_CHARGER_DONE = 3
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_CHARGER_DONE (3U)

/// saturated uint5 HOST_SLEEP_REASON_INACTIVITY_TIMEOUT = 4
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_INACTIVITY_TIMEOUT (4U)

/// saturated uint5 HOST_SLEEP_REASON_LOW_SOC = 5
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_LOW_SOC (5U)

/// saturated uint5 HOST_SLEEP_REASON_LOW_VOLTAGE = 6
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_LOW_VOLTAGE (6U)

/// saturated uint5 HOST_SLEEP_REASON_HOST_REQUEST = 7
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_HOST_REQUEST (7U)

/// saturated uint5 HOST_SLEEP_REASON_DIAGNOSTIC_DONE = 8
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_DIAGNOSTIC_DONE (8U)

/// saturated uint5 HOST_SLEEP_REASON_OTA_DONE = 9
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_OTA_DONE (9U)

/// saturated uint5 HOST_SLEEP_REASON_FAULT_LATCHED = 10
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_FAULT_LATCHED (10U)

/// saturated uint5 HOST_SLEEP_REASON_USER_REQUEST = 11
#define dsdl_Host_HostHeartbeat_1_0_HOST_SLEEP_REASON_USER_REQUEST (11U)

/// saturated uint5 HOST_WAKE_REASON_NONE = 0
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_NONE (0U)

/// saturated uint5 HOST_WAKE_REASON_ACC_ON = 1
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_ACC_ON (1U)

/// saturated uint5 HOST_WAKE_REASON_KEY_ON = 2
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_KEY_ON (2U)

/// saturated uint5 HOST_WAKE_REASON_CHARGER_PLUGGED = 3
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_CHARGER_PLUGGED (3U)

/// saturated uint5 HOST_WAKE_REASON_CAN_ACTIVITY = 4
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_CAN_ACTIVITY (4U)

/// saturated uint5 HOST_WAKE_REASON_LOCAL_IO = 5
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_LOCAL_IO (5U)

/// saturated uint5 HOST_WAKE_REASON_RTC_TIMER = 6
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_RTC_TIMER (6U)

/// saturated uint5 HOST_WAKE_REASON_HOST_REQUEST = 7
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_HOST_REQUEST (7U)

/// saturated uint5 HOST_WAKE_REASON_DIAGNOSTIC = 8
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_DIAGNOSTIC (8U)

/// saturated uint5 HOST_WAKE_REASON_FAULT = 9
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_FAULT (9U)

/// saturated uint5 HOST_WAKE_REASON_USER_REQUEST = 10
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_USER_REQUEST (10U)

/// saturated uint5 HOST_WAKE_REASON_POWER_ON_RESET = 11
#define dsdl_Host_HostHeartbeat_1_0_HOST_WAKE_REASON_POWER_ON_RESET (11U)

typedef struct
{
    /// saturated uint4 mode
    uint8_t mode;

    /// saturated uint2 health
    uint8_t health;

    /// saturated uint5 sleep_reason
    uint8_t sleep_reason;

    /// saturated uint5 wake_reason
    uint8_t wake_reason;

    /// saturated uint8 session_id
    uint8_t session_id;

    /// saturated uint8 detected_node_count
    uint8_t detected_node_count;

    /// saturated uint8 heartbeat_counter
    uint8_t heartbeat_counter;
} dsdl_Host_HostHeartbeat_1_0;

/// Serialize an instance into the provided buffer.
/// The lifetime of the resulting serialized representation is independent of the original instance.
/// This method may be slow for large objects (e.g., images, point clouds, radar samples), so in a later revision
/// we may define a zero-copy alternative that keeps references to the original object where possible.
///
/// @param obj      The object to serialize.
///
/// @param buffer   The destination buffer. There are no alignment requirements.
///                 @see dsdl_Host_HostHeartbeat_1_0_SERIALIZATION_BUFFER_SIZE_BYTES_
///
/// @param inout_buffer_size_bytes  When calling, this is a pointer to the size of the buffer in bytes.
///                                 Upon return this value will be updated with the size of the constructed serialized
///                                 representation (in bytes); this value is then to be passed over to the transport
///                                 layer. In case of error this value is undefined.
///
/// @returns Negative on error, zero on success.
static inline int8_t dsdl_Host_HostHeartbeat_1_0_serialize_(
    const dsdl_Host_HostHeartbeat_1_0* const obj, uint8_t* const buffer,  size_t* const inout_buffer_size_bytes)
{
    if ((obj == NULL) || (buffer == NULL) || (inout_buffer_size_bytes == NULL))
    {
        return -NUNAVUT_ERROR_INVALID_ARGUMENT;
    }
    const size_t capacity_bytes = *inout_buffer_size_bytes;
    if ((8U * (size_t) capacity_bytes) < 40UL)
    {
        return -NUNAVUT_ERROR_SERIALIZATION_BUFFER_TOO_SMALL;
    }
    // Notice that fields that are not an integer number of bytes long may overrun the space allocated for them
    // in the serialization buffer up to the next byte boundary. This is by design and is guaranteed to be safe.
    size_t offset_bits = 0U;
    {   // saturated uint4 mode
        uint8_t _sat0_ = obj->mode;
        if (_sat0_ > 15U)
        {
            _sat0_ = 15U;
        }
        buffer[offset_bits / 8U] = (uint8_t)(_sat0_);  // C std, 6.3.1.3 Signed and unsigned integers
        offset_bits += 4U;
    }
    {   // saturated uint2 health
        uint8_t _sat1_ = obj->health;
        if (_sat1_ > 3U)
        {
            _sat1_ = 3U;
        }
        const int8_t _err0_ = nunavutSetUxx(&buffer[0], capacity_bytes, offset_bits, _sat1_, 2U);
        if (_err0_ < 0)
        {
            return _err0_;
        }
        offset_bits += 2U;
    }
    {   // saturated uint5 sleep_reason
        uint8_t _sat2_ = obj->sleep_reason;
        if (_sat2_ > 31U)
        {
            _sat2_ = 31U;
        }
        const int8_t _err1_ = nunavutSetUxx(&buffer[0], capacity_bytes, offset_bits, _sat2_, 5U);
        if (_err1_ < 0)
        {
            return _err1_;
        }
        offset_bits += 5U;
    }
    {   // saturated uint5 wake_reason
        uint8_t _sat3_ = obj->wake_reason;
        if (_sat3_ > 31U)
        {
            _sat3_ = 31U;
        }
        const int8_t _err2_ = nunavutSetUxx(&buffer[0], capacity_bytes, offset_bits, _sat3_, 5U);
        if (_err2_ < 0)
        {
            return _err2_;
        }
        offset_bits += 5U;
    }
    {   // saturated uint8 session_id
        // Saturation code not emitted -- native representation matches the serialized representation.
        buffer[offset_bits / 8U] = (uint8_t)(obj->session_id);  // C std, 6.3.1.3 Signed and unsigned integers
        offset_bits += 8U;
    }
    {   // saturated uint8 detected_node_count
        // Saturation code not emitted -- native representation matches the serialized representation.
        buffer[offset_bits / 8U] = (uint8_t)(obj->detected_node_count);  // C std, 6.3.1.3 Signed and unsigned integers
        offset_bits += 8U;
    }
    {   // saturated uint8 heartbeat_counter
        // Saturation code not emitted -- native representation matches the serialized representation.
        buffer[offset_bits / 8U] = (uint8_t)(obj->heartbeat_counter);  // C std, 6.3.1.3 Signed and unsigned integers
        offset_bits += 8U;
    }
    if (offset_bits % 8U != 0U)  // Pad to 8 bits. TODO: Eliminate redundant padding checks.
    {
        const uint8_t _pad0_ = (uint8_t)(8U - offset_bits % 8U);
        const int8_t _err3_ = nunavutSetUxx(&buffer[0], capacity_bytes, offset_bits, 0U, _pad0_);  // Optimize?
        if (_err3_ < 0)
        {
            return _err3_;
        }
        offset_bits += _pad0_;
    }
    // It is assumed that we know the exact type of the serialized entity, hence we expect the size to match.
    *inout_buffer_size_bytes = (size_t) (offset_bits / 8U);
    return NUNAVUT_SUCCESS;
}

/// Deserialize an instance from the provided buffer.
/// The lifetime of the resulting object is independent of the original buffer.
/// This method may be slow for large objects (e.g., images, point clouds, radar samples), so in a later revision
/// we may define a zero-copy alternative that keeps references to the original buffer where possible.
///
/// @param obj      The object to update from the provided serialized representation.
///
/// @param buffer   The source buffer containing the serialized representation. There are no alignment requirements.
///                 If the buffer is shorter or longer than expected, it will be implicitly zero-extended or truncated,
///                 respectively; see Specification for "implicit zero extension" and "implicit truncation" rules.
///
/// @param inout_buffer_size_bytes  When calling, this is a pointer to the size of the supplied serialized
///                                 representation, in bytes. Upon return this value will be updated with the
///                                 size of the consumed fragment of the serialized representation (in bytes),
///                                 which may be smaller due to the implicit truncation rule, but it is guaranteed
///                                 to never exceed the original buffer size even if the implicit zero extension rule
///                                 was activated. In case of error this value is undefined.
///
/// @returns Negative on error, zero on success.
static inline int8_t dsdl_Host_HostHeartbeat_1_0_deserialize_(
    dsdl_Host_HostHeartbeat_1_0* const out_obj, const uint8_t* buffer, size_t* const inout_buffer_size_bytes)
{
    if ((out_obj == NULL) || (inout_buffer_size_bytes == NULL) || ((buffer == NULL) && (0 != *inout_buffer_size_bytes)))
    {
        return -NUNAVUT_ERROR_INVALID_ARGUMENT;
    }
    if (buffer == NULL)
    {
        buffer = (const uint8_t*)"";
    }
    const size_t capacity_bytes = *inout_buffer_size_bytes;
    const size_t capacity_bits = capacity_bytes * (size_t) 8U;
    size_t offset_bits = 0U;
    // saturated uint4 mode
    if ((offset_bits + 4U) <= capacity_bits)
    {
        out_obj->mode = buffer[offset_bits / 8U] & 15U;
    }
    else
    {
        out_obj->mode = 0U;
    }
    offset_bits += 4U;
    // saturated uint2 health
    out_obj->health = nunavutGetU8(&buffer[0], capacity_bytes, offset_bits, 2);
    offset_bits += 2U;
    // saturated uint5 sleep_reason
    out_obj->sleep_reason = nunavutGetU8(&buffer[0], capacity_bytes, offset_bits, 5);
    offset_bits += 5U;
    // saturated uint5 wake_reason
    out_obj->wake_reason = nunavutGetU8(&buffer[0], capacity_bytes, offset_bits, 5);
    offset_bits += 5U;
    // saturated uint8 session_id
    if ((offset_bits + 8U) <= capacity_bits)
    {
        out_obj->session_id = buffer[offset_bits / 8U] & 255U;
    }
    else
    {
        out_obj->session_id = 0U;
    }
    offset_bits += 8U;
    // saturated uint8 detected_node_count
    if ((offset_bits + 8U) <= capacity_bits)
    {
        out_obj->detected_node_count = buffer[offset_bits / 8U] & 255U;
    }
    else
    {
        out_obj->detected_node_count = 0U;
    }
    offset_bits += 8U;
    // saturated uint8 heartbeat_counter
    if ((offset_bits + 8U) <= capacity_bits)
    {
        out_obj->heartbeat_counter = buffer[offset_bits / 8U] & 255U;
    }
    else
    {
        out_obj->heartbeat_counter = 0U;
    }
    offset_bits += 8U;
    offset_bits = (offset_bits + 7U) & ~(size_t) 7U;  // Align on 8 bits.
    *inout_buffer_size_bytes = (size_t) (nunavutChooseMin(offset_bits, capacity_bits) / 8U);
    return NUNAVUT_SUCCESS;
}

/// Initialize an instance to default values. Does nothing if @param out_obj is NULL.
/// This function intentionally leaves inactive elements uninitialized; for example, members of a variable-length
/// array beyond its length are left uninitialized; aliased union memory that is not used by the first union field
/// is left uninitialized, etc. If full zero-initialization is desired, just use memset(&obj, 0, sizeof(obj)).
static inline void dsdl_Host_HostHeartbeat_1_0_initialize_(dsdl_Host_HostHeartbeat_1_0* const out_obj)
{
    if (out_obj != NULL)
    {
        size_t size_bytes = 0;
        const uint8_t buf = 0;
        const int8_t err = dsdl_Host_HostHeartbeat_1_0_deserialize_(out_obj, &buf, &size_bytes);

        (void) err;
    }
}

#ifdef __cplusplus
}
#endif
#endif // DSDL_HOST_HOST_HEARTBEAT_1_0_INCLUDED_

