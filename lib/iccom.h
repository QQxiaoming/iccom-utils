/**********************************************************************
* Copyright (c) 2021 Robert Bosch GmbH
* Artem Gulyaev <Artem.Gulyaev@de.bosch.com>
*
* This code is licensed under the Mozilla Public License Version 2.0
* License text is available in the file ’LICENSE.txt’, which is part of
* this source code package.
*
* SPDX-identifier: MPL-2.0
*
**********************************************************************/

/* This file provides the ICCom sockets driver convenience
 * interface to the user space programs, by avoiding a lot of
 * boiler plate in ICCom sockets communication establishing.
 */

#ifndef LIBICCOM_H
#define LIBICCOM_H

#include <linux/netlink.h>
#include <errno.h>

#ifdef __cplusplus
#include <vector>
#include <string>
#include <stdexcept>
#include <cassert>
#include <cstddef>
#else
#include <stddef.h>
#include <assert.h>
#endif

/* -------------------- BUILD TIME CONFIGURATION ----------------------- */

#define LIBICCOM_LOG_PREFIX "libiccom: "
// TODO: grab it from kernel header
#define ICCOM_LOOPBACK_IF_CTRL_FILE_PATH "/proc/iccomif/loopbackctl"
#define ICCOM_CHANNEL_AREA_PRIME 1
#define ICCOM_CHANNEL_AREA_LOOPBACK 2
#define ICCOM_CHANNEL_AREA_ANY 3

#define log(fmt, ...)                                                         \
    printf(LIBICCOM_LOG_PREFIX "%s: " fmt "\n", __func__, ## __VA_ARGS__);

// ICCom netlink family ID
#define NETLINK_ICCOM 22

// TODO: grab this information from kernel include
//  OR:  if we would have that in the device tree defined we could
//       just read a file in /sys that exposes this device tree attribute.
//       Or if its not an device tree attribute we could just
//       expose it via another procfs file to be read here
//
//       thanks to @Harald for the hint
#define ICCOM_SOCKET_MAX_MESSAGE_SIZE_BYTES 4096
// TODO: grab this information from kernel include
#define ICCOM_MIN_CHANNEL 0
// TODO: grab this information from kernel include
#define ICCOM_MAX_CHANNEL 0x7FFF

#define LUN_CID_2_CH(lun, cid)                  \
    ((((unsigned int)(lun)) << 7) | ((unsigned int)(cid)))

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------- ICCOM SOCKETS CONVENIENCE API ------------------- */

// the loopback configuration struct (see @iccom_loopback_enable
// for a bit more detailed explanation of fields)
//
// @from_ch the source channel region first channel
// @to_ch the source channel region last channel
// @range_shift the shift between destination region and the source
//  region
typedef struct loopback_cfg {
    unsigned int from_ch;
    unsigned int to_ch;
    int range_shift;
} loopback_cfg;

// RETURNS:
//      pointer to a const string to the name of the channel area
static inline const char* __iccom_ch_area_name(const int area_id)
{
    switch (area_id) {
    case ICCOM_CHANNEL_AREA_PRIME: return "prime";
    case ICCOM_CHANNEL_AREA_LOOPBACK: return "loopback";
    case ICCOM_CHANNEL_AREA_ANY: return "any";
    default: return "unknown";
    }
}


// Verifies the ch number for valid range (without loopback area)
// @channel ch number to verify
// @area identifies the area to check against
// @comment {NULL || valid str pointer} comment to the error log message
//    NOTE: if comment is NULL, then log is not printed
//
// RETURNS:
//      >= 0: when channel value is correct
//      < 0: when channel value is not correct
static inline int __iccom_channel_verify(const unsigned int channel
    , const int area, const char* const comment)
{
    if ((channel >= ICCOM_MIN_CHANNEL || channel <= ICCOM_MAX_CHANNEL)
        && (area == ICCOM_CHANNEL_AREA_PRIME
            || area == ICCOM_CHANNEL_AREA_ANY)) {
        return 0;
    }

    const int range_size = ICCOM_MAX_CHANNEL - ICCOM_MIN_CHANNEL + 1;
    if ((channel >= ICCOM_MIN_CHANNEL + range_size
            || channel <= ICCOM_MAX_CHANNEL + range_size)
        && (area == ICCOM_CHANNEL_AREA_LOOPBACK
            || area == ICCOM_CHANNEL_AREA_ANY)) {
        return 0;
    }

    if (!comment) {
        return -EINVAL;
    }

    if (strlen(comment)) {
        log("ch %d (%s) is out of %s ch range", channel
            , comment, __iccom_ch_area_name(area));
    } else {
        log("ch %d is out of %s ch range", channel
            , __iccom_ch_area_name(area));
    }
    return -EINVAL;
}

// verifies the validity of the ICCom channel value
// NOTE: ICCOM_MAX_CHANNEL + range_size is due to the loopback mapping
//      area which can be as big as original range
//      [ICCOM_MIN_CHANNEL; ICCOM_MAX_CHANNEL] and lay immediately
//      after the normal range:
//
//  ICCOM_MIN_CHANNEL   ICCOM_MIN_CHANNEL
//      |                   + range
//      |                      |
//      [-- normal channels --][ loopback channels -]
//                            |                     |
//                    ICCOM_MAX_CHANNEL     ICCOM_MAX_CHANNEL
//                                              + range
//      |<----- range ------->|
//
// RETURNS:
//      >= 0: when channel value is correct
//      < 0: when channel value is not correct
static inline int iccom_channel_verify(const int channel)
{
    const int range_size = ICCOM_MAX_CHANNEL - ICCOM_MIN_CHANNEL + 1;
    return (channel < ICCOM_MIN_CHANNEL
            || channel > ICCOM_MAX_CHANNEL + range_size)
           ? -EINVAL : 0;
}


// Prints the @data pointed data to the stdout in hex format.
//
// @data {valid data ptr || NULL} data to print out, if NULL
//      "<no data>" will be printed
// @len {>=0} the length of the data in bytes, if == 0,
//      "<no data>" will be printed
static inline void iccom_print_hex_dump(const void *const data, const size_t len)
{
    unsigned int i = 0;
    if (!data || !len) {
        printf("<no data>\n");
        return;
    }
    for (i = 0; i < len; i++) {
        printf("%#04x ", ((const char *const)data)[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
            if (i == len) {
                return;
            }
        }
    }
    printf("\n");
}

// Same as @iccom_print_hex_dump but puts a string @prefix to each
// output line (this is sometimes useful for readable output)
//
// @data {valid data ptr || NULL} data to print out, if NULL
//      "<no data>" will be printed with prefix
// @len {>=0} the length of the data in bytes, if == 0,
//      "<no data>" will be printed with prefix
// @prefix {valid null-terminated str ptr || NULL}
static inline void iccom_print_hex_dump_prefixed(const void *const data
        , const size_t len
        , const char* prefix)
{
    unsigned int i = 0;
    if (!prefix) {
        iccom_print_hex_dump(data, len);
        return;
    }
    if (!data || !len) {
        printf("%s<no data>\n", prefix);
        return;
    }
    printf("%s", prefix);
    for (i = 0; i < len; i++) {
        printf("%#04x ", ((const char *const)data)[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n%s", prefix);
            if (i == len) {
                return;
            }
        }
    }
    printf("\n");
}


// Opens the iccom socket to given channel.
//
// NOTE: by default socket has no timeout on receiving data operation,
//      to set the timeout please use @iccom_set_socket_read_timeout
//      call
//
// @channel {valid channel, see @iccom_channel_verify}
//      the channel to connect to
//
// RETURNS:
//      >=0: socket file descriptor , if socket
//          successfully opened
//      <0: negated error code, if fails
int iccom_open_socket(const unsigned int channel);

// Sets the socket read timeout.
//
// @sock_fd {a valid socket file descriptor}
// @ms >=0 timeout value in ms. If ms == 0, then
//     read operation will wait for data infinitely.
//
// RETURNS:
//      0: on success
//      <0: a negated error code
int iccom_set_socket_read_timeout(const int sock_fd, const int ms);

// Returns the current socket timeout value in ms
//
// RETURNS:
//      >= 0: socket read timeout value in m seconds
//      <0: if error occured
int iccom_get_socket_read_timeout(const int sock_fd);

// Sets the socket write timeout.
//
// @sock_fd {a valid socket file descriptor}
// @ms >=0 timeout value in ms. If ms == 0, then
//     write operation will wait for data infinitely.
//
// RETURNS:
//      0: on success
//      <0: a negated error code
int iccom_set_socket_write_timeout(const int sock_fd, const int ms);

// Returns the current socket timeout value in ms
//
// RETURNS:
//      >= 0: socket write timeout value in m seconds
//      <0: if error occured
int iccom_get_socket_write_timeout(const int sock_fd);

// Closes the iccom socket.
// @sock_fd {opened socket file descriptor} the descriptor validity
//      is checked by kernel
void iccom_close_socket(const int sock_fd);

// Sends the data to the given iccom socket efficiently.
// Uses the provided buffer to write the necessary data headers
// and padding to send the data to the socket immediately, without
// copying it to another buffer.
//
// @sock_fd {the valid file desctiptor} of iccom socket opened with
//      @open_iccom_socket(...).
// @buf {valid pointer to the send buffer} points to the buffer of
//      size @iccom_get_required_buffer_size(message to send length
//                                           in bytes)
//
//      buffer structure in general looks like following:
//
//      |--reserved space--|--payload data--|-padding-|
//
//      the reserved space will be used to create an appropriate
//      transport header before the data to avoid payload data
//      copying/moving.
//
//      padding is used to fit the message to 4 bytes length allignment
//
//      NOTE: the message (payload) data offset within the buffer can be
//          retrieved by @iccom_get_data_payload_offset() call.
// @buf_size_bytes { size of the buffer pointed by @buf in bytes}
//      MUST be equal to
//          iccom_get_required_buffer_size(message to send length in bytes)
//      NOTE: should be provided to ensure correct usage of the method
//          (in general can be deduced from @data_size_bytes)
// @data_offset {the user data (message) offset from the beginning of @buf}
//      must be equal to @iccom_get_data_payload_offset().
//      NOTE: should be provided to ensure correct usage of the method
//          (in general is a predefined constant)
// @data_size_bytes [1; @iccom_get_max_payload_size()]
//      the size of the user data (message) within the @buf buffer
//
// RETURNS:
//      0: on success
//      <0: negated error code, if fails
int iccom_send_data_nocopy(const int sock_fd, const void *const buf
               , const size_t buf_size_bytes
               , const size_t data_offset
               , const size_t data_size_bytes);


// Sends the data to the given iccom socket. Not efficient
// as long as it allocates buffer memory and performs memcpy
// of user provided data.
//
// @sock_fd {valid file desctiptor of iccom socket} opened with
//      @open_iccom_socket(...)
// @data {valid data ptr} the pointer to the user data to be sent.
// @data_size_bytes [1; @iccom_get_max_payload_size()]
//      the size of the user data (message) pointed by @data in bytes.
//
// RETURNS:
//      0: on success
//      <0: negated error code, if fails
int iccom_send_data(const int sock_fd, const void *const data
            , const  size_t data_size_bytes);

// RETURNS:
//      the offset of the consumer payload data in the buffer
//      which contains the full transportation ready message
//
//      NOTE: to clarify the usage, see @iccom_send_data_nocopy
//          description.
static inline size_t iccom_get_data_payload_offset(void)
{
    return (size_t)(NLMSG_LENGTH(0));
}

// RETURNS:
//      the size of the buffer which contains the full
//      transportation ready message for given payload
//      data size.
//
//      NOTE: to clarify the usage, see @iccom_send_data_nocopy
//          description.
static inline size_t iccom_get_required_buffer_size(
        const size_t data_size_bytes)
{
    return NLMSG_SPACE(data_size_bytes);
}

// RETURNS:
//      the maximal size of the consumer payload data to be
//      sent via single iccom_send_data...(...) call, in bytes.
static inline size_t iccom_get_max_payload_size(void)
{
    return ICCOM_SOCKET_MAX_MESSAGE_SIZE_BYTES;
}

// Waits&reads the data from iccom socket to the @receive_buffer efficiently.
// No memory allocations nor memory copying (except unavoidable kernel->user)
// is done.
//
// @sock_fd {valid, open iccom socket} the valid file desctiptor of
//      opened iccom socket (see @iccom_open_socket(...))
// @receive_buffer {!NULL} the valid pointer to the buffer of some
//      non-zero size (see below note).
//      NOTE: necessary size for given payload data can be determined by
//          calling
//          @iccom_get_required_buffer_size(message to send length in bytes)
//      NOTE: upon successfull read the buffer (due to performance reasons)
//          will contain not only the message data itself, but also
//          a netlink message header at the beginning and padding at the end:
//
//          |--reserved space--|--payload data--|-padding-|
//
//          payload data offset is provided via @data_offset__out parameter
//          payload data size is provided via return value
// @buffer_size {>0} the size of the buffer pointed by @receive_buffer
// @data_offset__out {!NULL} the pointer to the variable to which
//      the actual data offset in bytes (relative to the @receive_buffer) will
//      be written.
//      NOTE: Is set to defined value (user data offset) only when
//          read was successful and provided non-zero length data.
//
// RETURNS:
//      >=0: size of data (payload, customer data) received,
//           when succeeded,
//           NOTE: the timeout is not interpreted as an error
//              the 0 data size will be simply returned in case
//              of timeout.
//           NOTE:
//              ICCOM OVER TCP:
//                  + OR the socket has been closed, 0 also will be
//                    returned, and also timeout on socket read
//                    will not be respected.
//      <0: negated error code, when failed
//           NOTE: the timeout is not interpreted as an error
int iccom_receive_data_nocopy(
        const int sock_fd, void *const receive_buffer
        , const size_t buffer_size, int *const data_offset__out);

// Alias to @iccom_receive_data_nocopy(...) for now.
//
// TODO:
// NOTE: it is planned to make this function call to implement
//      delivery of pure user message data, without any transportation
//      headers. This will require to migrate all dependent projects.
int iccom_receive_data(const int sock_fd, void *const receive_buffer
               , const size_t buffer_size
               , int *const data_offset__out);

// NOTE: only for internal use for now. As it renamed, may be published.
int __iccom_receive_data_pure(const int sock_fd, void *const receive_buffer
                , const size_t buffer_size);

// The function enables the ICCom loopback for the given range of channels
// every channel #C end-point from range [from_ch; to_ch] will get connected
// to the channel end-point #(C + range_size) from range
// [from_ch + range_shift; to_ch + range_shift] in a bidirectional way.
//
// [from_ch; to_ch] <--------> [from_ch + range_shift; to_ch + range_shift]
// ^      ^       ^            ^                  ^                       ^
// | LOCAL END PT |            |          REMOTE END POINT                |
// \--------------\            /------------------------------------------/
//  \--------------\<-------->/------------------------------------------/
// |              |            |                                          |
// XXXXXXXXXXXXXXXX            XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// ----------------------- HW --------------------------------------------
// NOTE: THE HW PATHS ARE CUT OUT FROM SRC AND DST REGION
//
// @from_ch {valid, [ICCOM_MIN_CHANNEL; ICCOM_MAX_CHANNEL]} the source
//  channel region first channel
// @to_ch {valid, [from_ch; ICCOM_MAX_CHANNEL]} the source channel region
//  last channel
// @range_shift {valid} the shift between destination region and the source
//  region
//
// RETURNS:
//      >=0: on success
//      <0: on failure (negated error code)
int iccom_loopback_enable(const unsigned int from_ch, const unsigned int to_ch
              , const int range_shift);

// Disables the loopback of the channels see @iccom_loopback_enable.
//
// RETURNS:
//      >=0: on success
//      <0: on failure (negated error code)
int iccom_loopback_disable(void);

// Checks if loopback of the channels (see @iccom_loopback_enable) is enabled.
// RETURNS:
//      !0: loopback enabled
//      0: loopback is disabled / any error happened
char iccom_loopback_is_active(void);

// Get the loopback current configuration.
// @out {valid ptr to struct loopback_cfg) points to the struct where to
//  write the execution results.
//
// RETURNS:
//      >=0: all is OK (out data can be used only in this case)
//      <0: negated error code (out data is undefined)
int iccom_loopback_get(loopback_cfg *const out);


#ifdef __cplusplus
}
#endif

#ifdef __cplusplus


// keep the class in internal linkage by default, but if you are
// concerned with space in C++ program which has more than one
// translation unit which includes the libiccom, then you might
// want to
// * disable the class inclusion for all but one of your
//   translation units using the macro
//   LIBICCOM_CPP_WRAPPER_EXTERNAL for every translation unit
//   which is expected to use external cpp wrapper definition;
//   (marco to be defined before the inclusion of the libiccom.h
//   header).
// * set the LIBICCOM_CPP_WRAPPER_DEFINITION for the translation
//   unit, which shall contain the definition of the C++ wrapper.
#if !defined(LIBICCOM_CPP_WRAPPER_EXTERNAL) \
    && !defined(LIBICCOM_CPP_WRAPPER_DEFINITION)
namespace
{
#endif

// Convenience class to wrap raw ICCom API.
//
// CONCURRENCE:
//      class is not intended to be worked with from multiple
//      threads, methods are not reentrant nor multithreaded
//
// @m_sock_fd current file descriptor, if <0 then not opened
// @m_channel current channel for the socket, constant for given instance
// @m_incoming_data contains the received data including netlink header
//      and padding
//      NOTE: its reserved size is always NLMSG_SPACE(maximal message size)
//          to avoid reallocations at run time.
//      NOTE: if its actual size is 0, it means
//          "no data was received correctly"
// @m_outgoing_data contains the outgoing data including hetlink header
//      and padding
//      NOTE: its size never goes below NLMSG_SPACE(0).
//      NOTE: if its size is NLMSG_SPACE(0) this means that no output
//          data was provided yet
//      NOTE: its reserved size is always NLMSG_SPACE(maximal message size)
//          to avoid reallocations at run time.
//      NOTE: its size should be always equal to NLMSG_SPACE(current payload
//          data size to send)
// @m_outgoing_payload_size as long as @m_outgoing_data vector contains
//      padding for netlink allignment, we can not use its size to determine
//      actual size of the output data provided by user, so this variable
//      tracks the size of the data actually provided by user.
// @m_debug if true, then debug printing is enabled, otherwise - disabled
class IccomSocket
{
public:
    IccomSocket(const unsigned int channel);
    ~IccomSocket();

    int open();
    void close();

    bool is_open();
    inline unsigned int channel();

    int send(const bool reset_message_on_success = true);
    int receive() ;
    int send_direct(const std::vector<char> &data);
    int send_direct(const char *data, size_t len);
    int receive_direct(std::vector<char> &data_out);
    int receive_direct(void *const receive_buffer, const size_t buffer_size);

    int set_read_timeout(const int ms);
    int read_timeout();

    int set_write_timeout(const int ms);
    int write_timeout();

    void set_dbg_mode(const bool dbg_mode);

    void print_channel_data(const bool incoming
            , const std::string &prefix
                = std::string());

    static void print_channel_data_raw(
            const bool incoming
            , const void *const data
            , const size_t len
            , const unsigned int channel
            , const std::string &prefix);

    inline void reset_output();
    inline void reset_input();
    inline size_t output_size();
    inline size_t output_free_space();
    inline IccomSocket & operator <<(const char ch);
    inline IccomSocket & operator <<(
            const std::vector<char> &data);
    inline const char & operator[] (const size_t idx);
    inline size_t input_size();

private:
    int m_sock_fd;
    const unsigned int m_channel;
    std::vector<char> m_incoming_data;
    std::vector<char> m_outgoing_data;
    size_t m_outgoing_payload_size;
    bool m_dbg;
};

#ifndef LIBICCOM_CPP_WRAPPER_EXTERNAL
/* ----------------------- C++ class part ------------------------------ */

// Constructs the @IccomSocket for given channel but doesn't
// open it.
//
// DEFAULT STATE:
//      output data: empty
//      input data: empty
//      debug mode: disabled
//      socket: not opened
//
// THROWS:
//      std::out_of_range: when channel value is invalid
//      std::length_error: if requested buffer capacity is bigger
//          than max_size()
//      std::bad_alloc: if memory allocation for buffers fail
IccomSocket::IccomSocket(const unsigned int channel):
        m_sock_fd{-EAGAIN}
        , m_channel{channel}
        , m_incoming_data{}
        , m_outgoing_data{}
        , m_outgoing_payload_size{0}
        , m_dbg{false}
{
    this->m_sock_fd = -1;
    if (iccom_channel_verify(m_channel) < 0) {
        throw std::out_of_range("channel out of range");
    }
    this->m_incoming_data.reserve(
            NLMSG_SPACE(iccom_get_max_payload_size()));
    this->m_outgoing_data.reserve(
            NLMSG_SPACE(iccom_get_max_payload_size()));

    this->m_incoming_data.resize(0);
    this->m_outgoing_data.resize(NLMSG_SPACE(m_outgoing_payload_size));
}

// Closes the socket and destroys object related data.
IccomSocket::~IccomSocket()
{
    this->close();
}

// Opens the socket for corresponding channel.
// If socket is already opened: does nothing successfully.
//
// RETURNS:
//      >= 0: socket file descriptor, on success, includes
//          the case of already opened socket.
//      <0: on failure
int IccomSocket::open() 
{
    if (this->m_sock_fd >= 0) {
        return this->m_sock_fd;
    }
    this->m_sock_fd = iccom_open_socket(this->m_channel);
    return this->m_sock_fd;
}

// Closes the socket. If socket is already closed does nothing.
void IccomSocket::close() 
{
    if (this->m_sock_fd < 0) {
        return;
    }
    iccom_close_socket(this->m_sock_fd);
    this->m_sock_fd = -1;
}

// Returns the state of the socket
//
// RETURNS:
//      true: socket is opened
//      false: socket is not opened
bool IccomSocket::is_open() 
{
    return this->m_sock_fd >= 0;
}

// RETURNS:
//      the chanel number which is assigned to this instance
//      of IccomSocket
inline unsigned int IccomSocket::channel() 
{
    return this->m_channel;
}

// Sends current outgoing message.
//
// NOTE: the outgoing message can be written via << operator
//      on socket.
// NOTE: if outgoing message is empty, then just does nothing
//      and returns with success
//
// @reset_message_on_success
//      if true: (default) then if send was successful,
//          then outgoing message will be reset
//      if false: then if send was successful,
//          then outgoing message will remain
//
// NOTE: if send failed, then outgoing message will remain
//      in original shape (keep the same data) unless
//      manually erased by invocation of @reset_output
//
// RETURNS:
//      0: on success, inclusive no-data-to-send case
//      <0: negated error code, if fails
int IccomSocket::send(const bool reset_message_on_success) 
{
    if (m_outgoing_payload_size == 0) {
        return 0;
    }
    int res = iccom_send_data_nocopy(
            this->m_sock_fd
            , this->m_outgoing_data.data()
            , this->m_outgoing_data.size()
            , NLMSG_LENGTH(0)
            , m_outgoing_payload_size);
    if (res < 0) {
        return res;
    }
    if (this->m_dbg) {
        print_channel_data(false, "    [RCV]:");
    }
    if (reset_message_on_success) {
        this->reset_output();
    }
    return res;
}

// Receives the data into the socket buffer. Blocks
// until data received or timeout happens (latter if configured).
//
// If successfull the received message will be available
// via [] operator on socket, and its length given by
// @input_size() will be > 0.
//
// On failure, input_size() will return 0 input message size.
//
// RETURNS:
//      see @iccom_receive_data_nocopy description
int IccomSocket::receive() 
{
    // NOTE: unless some magic happenes for memory
    //      allocation/freeing policy, this resize
    //      will do nothing more than size value assignment
    m_incoming_data.resize(NLMSG_SPACE(
            iccom_get_max_payload_size()));

    int data_offset = 0;
    int res = iccom_receive_data_nocopy(
            this->m_sock_fd
            , m_incoming_data.data()
            , m_incoming_data.size()
            , &data_offset);
    // == 0 case includes the timeout case
    if (res <= 0) {
        reset_input();
        return res;
    }
    // unexpected offset case, should not happen
    if (data_offset != NLMSG_LENGTH(0)) {
        reset_input();
        return -EFAULT;
    }
    m_incoming_data.resize(NLMSG_LENGTH(res));

    if (this->m_dbg) {
        print_channel_data(true, "    [RCV]:");
    }

    return res;
}

// Wrapper of @iccom_send_data for current channel
//
// NOTE: not efficient, only to use in occasional
//      cases, for efficient implementation use @send()
//
// RETURNS:
//      0: on success
//      <0: negated error code, if fails
int IccomSocket::send_direct(
        const std::vector<char> &data) 
{
    if (!this->is_open()) {
           return -EBADFD;
    }
    return iccom_send_data(this->m_sock_fd, data.data(), data.size());
}

// Wrapper of @iccom_send_data for current channel
//
// NOTE: not efficient, only to use in occasional
//      cases, for efficient implementation use @send()
//
// RETURNS:
//      0: on success
//      <0: negated error code, if fails
int IccomSocket::send_direct(const char *data, size_t len)
{
    if (!this->is_open()) {
           return -EBADFD;
    }
    return iccom_send_data(this->m_sock_fd, data, len);
}

// Wrapper of @__iccom_receive_data_pure for current channel
//
// @data_out will be resized to 0 in case of failure,
//      will contain user message in case of success.
//
// RETURNS:
//      0: on success
//      <0: negated error code, if fails
int IccomSocket::receive_direct(
        std::vector<char> &data_out) 
{
    if (!this->is_open()) {
        data_out.resize(0);
        return -EBADFD;
    }

    data_out.resize(NLMSG_SPACE(iccom_get_max_payload_size()));
    int res = __iccom_receive_data_pure(this->m_sock_fd, data_out.data()
                        , data_out.size());
    if (res < 0) {
        data_out.resize(0);
        return res;
    }
    data_out.resize(res);
    return res;
}

// Wrapper of @__iccom_receive_data_pure for current channel
//
// @data_out will be resized to 0 in case of failure,
//      will contain user message in case of success.
//
// RETURNS:
//      0: on success
//      <0: negated error code, if fails
int IccomSocket::receive_direct(void *const receive_buffer, const size_t buffer_size)
{
    if (!this->is_open()) {
        return -EBADFD;
    }

    return __iccom_receive_data_pure(this->m_sock_fd, receive_buffer
                        , buffer_size);
}


// Sets the socket read timeout.
// Wrapper around @iccom_set_socket_read_timeout(...)
//
// @ms >=0 timeout value in ms. If ms == 0, then
//     read operation will wait for data infinitely.
//
// RETURNS:
//      0: on success
//      <0: a negated error code
int IccomSocket::set_read_timeout(const int ms) 
{
    if (!is_open()) {
        return -EBADF;
    }
    return iccom_set_socket_read_timeout(this->m_sock_fd, ms);
}

// Returns the current socket timeout value in ms.
// Wrapper around @iccom_get_socket_read_timeout(...)
//
// RETURNS:
//      >=0: on success, the timeout value in msecs
//          NOTE: 0 means no timeout for read operation
//      <0: on failure
int IccomSocket::read_timeout() 
{
    if (!is_open()) {
        return -EBADF;
    }
    return iccom_get_socket_read_timeout(this->m_sock_fd);
}

// Sets the socket write timeout.
// Wrapper around @iccom_set_socket_write_timeout(...)
//
// @ms >=0 timeout value in ms. If ms == 0, then
//     write operation will wait for data infinitely.
//
// RETURNS:
//      0: on success
//      <0: a negated error code
int IccomSocket::set_write_timeout(const int ms) 
{
    if (!is_open()) {
        return -EBADF;
    }
    return iccom_set_socket_write_timeout(this->m_sock_fd, ms);
}

// Returns the current socket timeout value in ms.
// Wrapper around @iccom_get_socket_write_timeout(...)
//
// RETURNS:
//      >=0: on success, the timeout value in msecs
//          NOTE: 0 means no timeout for write operation
//      <0: on failure
int IccomSocket::write_timeout() 
{
    if (!is_open()) {
        return -EBADF;
    }
    return iccom_get_socket_write_timeout(this->m_sock_fd);
}

// Sets the debug printing mode.
//
// In dbg mode on every receive/send the corresponding
// user message data is printed to stdout.
//
// @dbg_mode if true: the dbg mode to be enabled
//      if false: the dbg mode to be disabled
void IccomSocket::set_dbg_mode(const bool dbg_mode)
{
    this->m_dbg = dbg_mode;
}

// Prints out to stdout the contents of current
// incoming or current outgoing message
// (for debug purposes)
//
// @incoming if true, print the current incoming data,
//      otherwise, print the current outgoing data
// @prefix {any} the logging prefix string to use
void IccomSocket::print_channel_data(const bool incoming
        , const std::string &prefix)
{
    if (incoming) {
        const size_t size = input_size();
        if (size == 0) {
            printf("%sno input data on channel %d\n"
                   , prefix.data(), m_channel);
            return;
        }
        assert(m_incoming_data.size() >= NLMSG_SPACE(size));
        print_channel_data_raw(true
                , NLMSG_DATA(m_incoming_data.data())
                , size, m_channel, prefix);
        return;
    }

    assert(m_outgoing_data.size()
            == NLMSG_SPACE(m_outgoing_payload_size));

    if (m_outgoing_payload_size == 0) {
        printf("%sno output data on channel %d\n"
               , prefix.data(), m_channel);
        return;
    }
    print_channel_data_raw(false, NLMSG_DATA(m_outgoing_data.data())
                   , m_outgoing_payload_size
                   , m_channel, prefix);
}

// Prints out to stdout the contents of given incoming
// or outgoing message (for debug purposes)
//
// @incoming if true, print the data as incoming data,
//      otherwise, as outgoing data
// @data {valid data ptr || NULL} pointer to the data
//      to print out
// @len {length of @data}
// @channel the channel number to print out
// @prefix {any} the logging prefix string to use
void IccomSocket::print_channel_data_raw(
        const bool incoming
        , const void *const data
        , const size_t len
        , const unsigned int channel
        , const std::string &prefix)
{
    if (!data || !len) {
        printf("%sno %s data on channel %d\n"
               , prefix.data()
               , incoming ? "input" : "outgoing"
               , channel);
        return;
    }

    if (incoming) {
        printf("%s[RCV] ch %d; %zu bytes --- payload data begin ---\n"
               , prefix.data(), channel, len);
        iccom_print_hex_dump_prefixed(data, len, prefix.data());
        printf("%sch %d; %zu bytes --- payload data end   ---\n"
               , prefix.data(), channel, len);
        return;
    }

    printf("%s[SND] ch %d; %zu bytes --- payload data begin ---\n"
           , prefix.data(), channel, len);
    iccom_print_hex_dump_prefixed(data, len, prefix.data());
    printf("%sch %d; %zu bytes --- payload data end   ---\n"
           , prefix.data(), channel, len);
}

// Resets the output buffer efficiently, so the next write
// will be at the beginning of the data to send.
inline void IccomSocket::reset_output() 
{
    m_outgoing_data.resize(NLMSG_SPACE(0));
    m_outgoing_payload_size = 0;
}

// Resets the input buffer efficiently. This is only
// to track/mark the incoming message as "done", and
// will not affect the socket work anyhow.
inline void IccomSocket::reset_input() 
{
    m_incoming_data.resize(0);
}

// RETURNS:
//      the current size of outgoing message (in bytes)
//      NOTE: only raw consumer data is taken into account
inline size_t IccomSocket::output_size() 
{
    return m_outgoing_payload_size;
}

// RETURNS:
//      the size of the free space available for the outgoing message
//      (in bytes)
inline size_t IccomSocket::output_free_space() 
{
    const size_t max_s = iccom_get_max_payload_size();
    const size_t curr_s = m_outgoing_payload_size;

    return max_s >= curr_s ? (max_s - curr_s) : 0;
}

// Writes a single character to the output message
// NOTE:
//      if message already reached the maximum size, then is
//      does nothing.
// NOTE: use @output_free_space() to get the free space available
inline IccomSocket & IccomSocket::operator <<(const char ch) 
{
    // we can not write more data
    if (m_outgoing_payload_size >= iccom_get_max_payload_size()) {
        return *this;
    }
    m_outgoing_data.push_back(ch);
    m_outgoing_payload_size++;
    // NOTE: this resize is needed, cause padding can be added
    //      to the end of the buffer after one character addition.
    m_outgoing_data.resize(NLMSG_SPACE(m_outgoing_payload_size));
    return *this;
}

// Writes a provided data to the output message
// NOTE:
//      if new data is too big to fit the max message size
//      then it does nothing.
// NOTE: use @output_free_space() to get the free space available
inline IccomSocket & IccomSocket::operator <<(
        const std::vector<char> &data)
{
    // we can not write more data
    if (m_outgoing_payload_size + data.size()
            >= iccom_get_max_payload_size()) {
        return *this;
    }
    for (auto ch : data) {
        this->m_outgoing_data.push_back(ch);
    }
    m_outgoing_payload_size += data.size();
    m_outgoing_data.resize(NLMSG_SPACE(m_outgoing_payload_size));
    return *this;
}

// Indexes the incoming message in readonly mode.
// @idx [0; input_size() - 1] otherwise you will face undefined
//      behaviour
//
// RETURNS:
//      the reference to the incoming data payload character
inline const char & IccomSocket::operator[] (const size_t idx) 
{
    assert(idx >= 0 && idx < input_size());
    return m_incoming_data[idx + NLMSG_LENGTH(0)];
}

// RETURNS:
//      the size of current incoming user message (in bytes)
//      NOTE: only raw consumer data is taken into account
inline size_t IccomSocket::input_size() 
{
    auto nlmsghdr = (struct nlmsghdr*)(
                this->m_incoming_data.data());
    return (this->m_incoming_data.size() >= NLMSG_SPACE(0))
        ? (NLMSG_PAYLOAD(nlmsghdr, 0))
        : 0;
}
#endif // ifndef LIBICCOM_CPP_WRAPPER_EXTERNAL

#if !defined(LIBICCOM_CPP_WRAPPER_EXTERNAL) \
    && !defined(LIBICCOM_CPP_WRAPPER_DEFINITION)
} // end of unnamed namespace
#endif

#endif //ifdef __cplusplus

#endif //ifndef LIBICCOM_H
