/*
 * This file defines the Inter Chip/CPU communication protocol (ICCom)
 * driver for communication between two CPUs based on full duplex
 * fully symmetrical transport layer (like one provided by SymSPI).
 *
 * Copyright (c) 2020 Robert Bosch GmbH
 * Artem Gulyaev <Artem.Gulyaev@de.bosch.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// SPDX-License-Identifier: GPL-2.0

// ICCom protocol overiview is the following:
//
// * based on full-duplex and fully symmetrical transport layer
//   (like SymSPI)
// * single transmission frame consists of two steps:
//   * data package transmission (in both directions)
//   * ack package transmission (in both directions)
// * data package (is of fixed size) consists of
//   * header
//   * payload
//   * CRC32 control field
// * ack package is just a single predefined byte which acks the
//   transmission (or not acks, if not equal to ack byte)
// * if data package is not acked, then it shall be resent in the next
//   frame
// * package payload contains packets
// * every packet consists of
//   * header (defines the destination address of the payload and its size)
//   * payload itself
//
// that is it.

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/hrtimer.h>
#include <linux/uaccess.h>

#include "full_duplex_interface.h"
#include "iccom.h"

/* --------------------- BUILD CONFIGURATION ----------------------------*/

// package layout info, see @iccom_package description
// (all sizes in bytes)

// Defines the log verbosity level for ICCom
// 0: total silence
// 1: only error messages
// 2: + warnings
// 3: (DEFAULT) + key info messages (info level 0)
// 4: + optional info messages (info level 1)
// 5: + all info messages (debug information) (info level 2)
//    NOTE: automatically enables debug mode at 5 level of
//    verbosity
#define ICCOM_VERBOSITY 3

// The minimal time which must pass between repeated error is reported
// to avoid logs flooding.
// 0: no minimal interval
// >0: minimal time interval in mseconds
#define ICCOM_MIN_ERR_REPORT_INTERVAL_MSEC 10000
// The rate (number of msec) at which the error rate decays double.
// The error rate is relevant in interpretation of the errors,
// cause occasional errors usually don't have very high significance,
// while high error rate usually indicates a real fault.
// Surely: > 0
#define ICCOM_ERR_RATE_DECAY_RATE_MSEC_PER_HALF 2000
// Minimal decay rate, even if error events are sequential
#define ICCOM_ERR_RATE_DECAY_RATE_MIN 3

// All debug macro can be set via kernel config
#if ICCOM_VERBOSITY >= 5
#define ICCOM_DEBUG
#endif

#ifdef ICCOM_DEBUG
// -1 means "all"
#ifndef ICCOM_DEBUG_CHANNEL
#define ICCOM_DEBUG_CHANNEL -1
#endif
// -1 means "unlimited", 0 means "do not print"
#ifndef ICCOM_DEBUG_MESSAGES_PRINTOUT_MAX_COUNT
#define ICCOM_DEBUG_MESSAGES_PRINTOUT_MAX_COUNT 5
#endif
// -1 means "unlimited", 0 means "do not print"
#ifndef ICCOM_DEBUG_PACKAGES_PRINT_MAX_COUNT
#define ICCOM_DEBUG_PACKAGES_PRINT_MAX_COUNT 5
#endif
#endif /* ICCOM_DEBUG */

#define ICCOM_LOG_PREFIX "ICCom: "

// Selects the workqueue to use to run consumer delivery operations
// to not to block the underlying transport layer.
//
// Three options are available now:
// * "ICCOM_WQ_SYSTEM": see system_wq in workqueue.h.
// * "ICCOM_WQ_SYSTEM_HIGHPRI": see system_highpri_wq in
//   workqueue.h.
// * "ICCOM_WQ_PRIVATE": use privately constructed single threaded
//   workqueue.
//
// NOTE: the selection of the workqueue depends on the
//      generic considerations on ICCom functioning
//      within the overall system context. Say if ICCom serves
//      as a connection for an optional device, or device which
//      can easily wait for some sec (in the worst case) to react
//      then "ICCOM_WQ_SYSTEM" workqeue is a nice option to select.
//      On the other hand if no delays are allowed in handling ICCom
//      communication (say, to communicate to hardware watchdog)
//      then "ICCOM_WQ_SYSTEM_HIGHPRI" or "ICCOM_WQ_PRIVATE" is
//      surely more preferrable.
//
// Can be set via kernel config, see:
// 		BOSCH_ICCOM_WORKQUEUE_MODE configuration parameter
#ifndef ICCOM_WORKQUEUE_MODE
#define ICCOM_WORKQUEUE_MODE ICCOM_WQ_PRIVATE
#endif

#define ICCOM_WQ_SYSTEM 0
#define ICCOM_WQ_SYSTEM_HIGHPRI 1
#define ICCOM_WQ_PRIVATE 2

// Comparator
#define ICCOM_WORKQUEUE_MODE_MATCH(x)		\
	ICCOM_WORKQUEUE_MODE == ICCOM_WQ_##x

// the number alias to workqueue mode
#ifndef ICCOM_WORKQUEUE_MODE
#error ICCOM_WORKQUEUE_MODE must be defined to \
		one of [ICCOM_WQ_SYSTEM, ICCOM_WQ_SYSTEM_HIGHPRI, \
		ICCOM_WQ_PRIVATE].
#endif


// DEV STACK
// @@@@@@@@@@@@@
//
// Verification:
//      * Handle transport device transport error by restarting
//        the frame, otherwise the communication will halt every
//        time other side indicates an error.
//
// WIP:
//      * verbosity levels smooth adjustment
//
// BACKLOG:
//
//      * TODO in iccom_close
//
//      * allow socket callbacks definition hierarchy and mapping
//        PER CHANNEL END:
//
//                Ch#1clb      Ch#5clb
//                  |            |
//
//          |---Callb.1--map-||--Clb.2-map--|   |--Callb.1--map-|
//
//          |----------- global callback (for all channels) -------|
//
//          GLOBAL END
//
//      * set MAX number of bytes per channel in message storage and
//        drop with new incoming messages otherwise we might end up
//        in message storage uncontrollable expansion due to forgotten
//        /unprocessed messages.
//
//      * incremental CRC32 computation
//
//      * kmem_cache_free check if neede (review)
//
//      * THE MINIMAL PACKAGE FILLING UP RATIO INTRODUCTION (to avoid
//        almost empty packages to be sent to the other side)
//           + introduction of priority messages (which trigger the
//             package send even it is almost empty)
//
//      * fixing writing package payload length from
//          adding the byte-bit endianness transformation procedures
//              V __iccom_package_set_payload_size
//              V __iccom_package_payload_size
//              __iccom_package_set_src
//              __iccom_package_get_src
//
//      * fixing __iccom_packet_parse_into_struct
//          ver iccom_packet_get_channel
//
//      * ver __iccom_read_next_packet
//        for package 00 01 00 05 23 45 32 ff ff
//          ver __iccom_packet_parse_into_struct
//
//      * ADDING THREAD SAFETY TO THE ICCOM MESSAGE STORAGE
//          ver. __iccom_msg_storage_allocate_next_msg_id
//              CORRECTING __iccom_msg_storage_allocate_next_msg_id USAGE
//                  ver. iccom_msg_storage_push_message usage
//                      ver __iccom_construct_message_in_storage usage
//                          ver __iccom_read_next_packet usage
//
//      * ver __iccom_process_package_payload and usage
//
//      * Condenced printing
//
//      * Add "do {.....} while (0)" for all function-like macro
//
//      * make statistics robust
//
//      * verify the reason of crash if uncomment the following line
//        (around 2910 line):
//           __iccom_msg_storage_printout(&iccom->p->rx_messages);
//
//      * __iccom_enqueue_new_tx_data_package might better to return
//        pointer to the created package or error-pointer in case of
//        errors
//
//      * ALLOCATE WHOLE MESSAGE SIZE IMMEDIATELY WITH THE FIRST BLOCK
//        OF THE MESSAGE
//
//      * ADD INITIAL EMPTY PACKAGE ON INIT
//
//      * TO VERIFY: MESSAGES with finalized == false
//        || uncommitted_length will never be
//              delivered/amened/deleted to/by consumer
//
//      * CONST REF =)
//
//      * TECHNICALLY WE CAN LEAVE THE XFER DATA UNTOUCHED, and simply
//        generate the memory regions list for every message, like
//        following
//
//        |--------------XFER---------------|
//           |MSG1 part1  | MSG2  part 3|
//
//        MSG1: part 1 ptr + size; part 2 ptr + size, part 3 ptr + size;
//        MSG2: part 1 ptr + size; part 2 ptr + size, part 3 ptr + size;...
//
//
//      * TO THINK ABOUT BANDWIDTH QUOTES/LOAD BALANCING, to make consumers
//        to have bandwidth preallocated and thus guaranteed to avoid the
//        situations when single consumer consumes whole bandwidth of the
//        whole device.
//
//      * BANDWIDTH ALLOCATION CAN BE MADE BY PRIORITIZATION of incoming
//        packets (the more bytes is sent, the less priority gets)
//
//      * if callback thread is blocked for more than gitven threshold,
//        then it is reasonable to launch the second worker thread by
//        timeout to 1) avoid excessive threads creation 2) still be able
//        to avoid one single consumer callback to block the whole ICCom
//        callback path
//
//      * Add maximum message sending attempt (not totally sure if it is
//        needed)
//
// @@@@@@@@@@@@@

/* --------------------- GENERAL CONFIGURATION --------------------------*/

// TODO: consider
// This channel ID is used to xfer ICCom technical information
// to the other side ICCom.
#define ICCOM_TECHNICAL_CHANNEL_ID 0

// should be > 0
#define ICCOM_INITIAL_PACKAGE_ID 1

// the root directory in proc file system, which contains
// ICCom information for user space
#define ICCOM_PROC_ROOT_NAME "iccom"
// the name of the character device to readout ICCom statistics
#define ICCOM_STATISTICS_FILE_NAME "statistics"
#define ICCOM_PROC_R_PERMISSIONS 0444

#if ICCOM_DATA_XFER_SIZE_BYTES > ICCOM_ACK_XFER_SIZE_BYTES
#define ICCOM_BUFFER_SIZE ICCOM_DATA_XFER_SIZE_BYTES
#else
#define ICCOM_BUFFER_SIZE ICCOM_ACK_XFER_SIZE_BYTES
#endif

/* --------------------- DATA PACKAGE CONFIGURATION ---------------------*/

// unused payload space filled with this value
#define ICCOM_PACKAGE_EMPTY_PAYLOAD_VALUE 0xFF
#define ICCOM_PACKAGE_PAYLOAD_DATA_LENGTH_FIELD_SIZE_BYTES 2
#define ICCOM_PACKAGE_ID_FIELD_SIZE_BYTES 1
#define ICCOM_PACKAGE_CRC_FIELD_SIZE_BYTES 4

// packet layout info (all sizes in bytes), see
//      SALT documentation, 20 November 2018
//          , 1.4.4 Payload data organization
//      blocks: Table 11, Table 13.
#define ICCOM_PACKET_HEADER_PAYLOAD_SIZE_FIELD_SIZE_BYTES 2
#define ICCOM_PACKET_HEADER_LUN_FIELD_SIZE_BYTES 1
#define ICCOM_PACKET_HEADER_CID_COMPLETE_FIELD_SIZE_BYTES 1

#define ICCOM_PACKET_HEADER_SIZE_BYTES					\
	(ICCOM_PACKET_HEADER_PAYLOAD_SIZE_FIELD_SIZE_BYTES		\
	 + ICCOM_PACKET_HEADER_LUN_FIELD_SIZE_BYTES			\
	 + ICCOM_PACKET_HEADER_CID_COMPLETE_FIELD_SIZE_BYTES)

/* ---------------------- ACK PACKAGE CONFIGURATION ---------------------*/
#define ICCOM_PACKAGE_ACK_VALUE 0xD0
#define ICCOM_PACKAGE_NACK_VALUE 0xE1

/* ---------------------- ADDITIONAL VALUES -----------------------------*/

#define ICCOM_PACKET_INVALID_CHANNEL_ID -1
#define ICCOM_PACKET_MIN_CHANNEL_ID 0
#define ICCOM_PACKET_MAX_CHANNEL_ID 0x7FFF
#define ICCOM_PACKET_INVALID_MESSAGE_ID 0
#define ICCOM_PACKET_INITIAL_MESSAGE_ID 1

/* --------------------- UTILITIES SECTION ----------------------------- */

#if ICCOM_VERBOSITY >= 1
#define iccom_err(fmt, ...)						\
	pr_err(ICCOM_LOG_PREFIX"%s: "fmt"\n", __func__, ##__VA_ARGS__)
#define iccom_err_raw(fmt, ...)						\
	pr_err(ICCOM_LOG_PREFIX""fmt"\n", ##__VA_ARGS__)
#else
#define iccom_err(fmt, ...)
#define iccom_err_raw(fmt, ...)
#endif

#if ICCOM_VERBOSITY >= 2
#define iccom_warning(fmt, ...)						\
	pr_warning(ICCOM_LOG_PREFIX"%s: "fmt"\n", __func__		\
		   , ##__VA_ARGS__)
#define iccom_warning_raw(fmt, ...)					\
	pr_warning(ICCOM_LOG_PREFIX""fmt"\n", ##__VA_ARGS__)
#else
#define iccom_warning(fmt, ...)
#define iccom_warning_raw(fmt, ...)
#endif

#if ICCOM_VERBOSITY >= 3
#define iccom_info_helper(fmt, ...)					\
	pr_info(ICCOM_LOG_PREFIX"%s: "fmt"\n", __func__, ##__VA_ARGS__)
#define iccom_info_raw_helper(fmt, ...)					\
	pr_info(ICCOM_LOG_PREFIX""fmt"\n", ##__VA_ARGS__)
#define iccom_info_helper_0(fmt, ...)					\
	iccom_info_helper(fmt, ##__VA_ARGS__)
#define iccom_info_raw_helper_0(fmt, ...)				\
	iccom_info_raw_helper(fmt, ##__VA_ARGS__)
#else
#define iccom_info_helper(fmt, ...)
#define iccom_info_raw_helper(fmt, ...)
#define iccom_info_helper_0(fmt, ...)
#define iccom_info_raw_helper_0(fmt, ...)
#endif

#if ICCOM_VERBOSITY >= 4
#define iccom_info_helper_1(fmt, ...)					\
	iccom_info_helper(fmt, ##__VA_ARGS__)
#define iccom_info_raw_helper_1(fmt, ...)				\
	iccom_info_raw_helper(fmt, ##__VA_ARGS__)
#else
#define iccom_info_helper_1(fmt, ...)
#define iccom_info_raw_helper_1(fmt, ...)
#endif

#if ICCOM_VERBOSITY >= 5
#define iccom_info_helper_2(fmt, ...)					\
	iccom_info_helper(fmt, ##__VA_ARGS__)
#define iccom_info_raw_helper_2(fmt, ...)				\
	iccom_info_raw_helper(fmt, ##__VA_ARGS__)
#else
#define iccom_info_helper_2(fmt, ...)
#define iccom_info_raw_helper_2(fmt, ...)
#endif

// information messages levels
#define ICCOM_LOG_INFO_KEY_LEVEL 0
#define ICCOM_LOG_INFO_OPT_LEVEL 1
#define ICCOM_LOG_INFO_DBG_LEVEL 2

#define iccom_info_helper__(level, fmt, ...)				\
	iccom_info_helper_##level(fmt, ##__VA_ARGS__)
#define iccom_info_raw_helper__(level, fmt, ...)			\
	iccom_info_raw_helper_##level(fmt, ##__VA_ARGS__)

#define iccom_info(level, fmt, ...)					\
	iccom_info_helper__(level, fmt, ##__VA_ARGS__)
#define iccom_info_raw(level, fmt, ...)					\
	iccom_info_raw_helper__(level, fmt, ##__VA_ARGS__)


#define ICCOM_CHECK_DEVICE(msg, error_action)				\
	if (IS_ERR_OR_NULL(iccom)) {					\
		iccom_err("%s: no device; "msg"\n", __func__);		\
		error_action;						\
	}
#define ICCOM_CHECK_DEVICE_PRIVATE(msg, error_action)			\
	if (IS_ERR_OR_NULL(iccom->p)) {					\
		iccom_err("%s: no private part of device; "msg"\n"	\
			  , __func__);					\
		error_action;						\
	}
#define ICCOM_CHECK_CLOSING(msg, closing_action)			\
	if (iccom->p->closing) {					\
		iccom_warning("%s: device is closing; "msg"\n"		\
			      , __func__);				\
		closing_action;						\
	}
#define ICCOM_CHECK_CHANNEL_EXT(channel, msg, error_action)		\
	if ((channel < ICCOM_PACKET_MIN_CHANNEL_ID			\
			|| channel > ICCOM_PACKET_MAX_CHANNEL_ID)	\
		    && channel != ICCOM_ANY_CHANNEL_VALUE) {		\
		iccom_err("%s: bad channel; "msg"\n", __func__);	\
		error_action;						\
	}
#define ICCOM_CHECK_CHANNEL(msg, error_action)				\
	ICCOM_CHECK_CHANNEL_EXT(channel, msg, error_action);

#define ICCOM_CHECK_PTR(ptr, error_action)				\
	if (IS_ERR_OR_NULL(ptr)) {					\
		iccom_err("%s: pointer "# ptr" is invalid;\n"		\
			  , __func__);					\
		error_action;						\
	}

#define ICCOM_MSG_STORAGE_CHECK_STORAGE(msg, error_action)		\
	if (IS_ERR_OR_NULL(storage)) {					\
		iccom_err("%s: bad msg storage ptr; "msg"\n", __func__);\
		error_action;						\
	}

#define __iccom_err_report(err_no, sub_error_no)			\
	__iccom_error_report(iccom, err_no, sub_error_no, __func__)

/* ------------------------ FORWARD DECLARATIONS ------------------------*/

struct full_duplex_xfer *__iccom_xfer_failed_callback(
		const struct full_duplex_xfer __kernel *failed_xfer
		, const int next_xfer_id
		, int error_code
		, void __kernel *consumer_data);
struct full_duplex_xfer *__iccom_xfer_done_callback(
			const struct full_duplex_xfer __kernel *done_xfer
			, const int next_xfer_id
			, bool __kernel *start_immediately__out
			, void *consumer_data);
static ssize_t __iccom_statistics_read(struct file *file
		, char __user *ubuf
		, size_t count
		, loff_t *_unused_ppos);

/* --------------------------- MAIN STRUCTURES --------------------------*/

// TODO: probably not needed
// (probably needed only for xfer)
//
// Describes the single consumer message.
//
// @list_anchor messages list anchor point
// @data the consumer raw byte data. Always owns the data.
// @length size of payload in @data in bytes (while message is
//      unfinished keeps the current data length)
//      NOTE: the @length may be less than allocated @data size
//          due to rollback of uncommitted changes.
// @channel the id of the channel the message is assigned to.
// @id the sequential message id (assigned by iccom to uniquely
//      identify the message among others within adequate time
//      frame). @id is wrapped around so it will be unique only
//      within some message relevance time frame, but not globally.
//
//      NOTE: in legacy ICCom implementation is not used.
//
//      NOTE(TODO): while the field is not used, we will use message
//          id always equal to 0
//
// @priority the message priority value which indicates the importance
//      of fast delivery of the message. The higher the value, the
//      faster the message needs to be delivered.
//      0 - background xfer (should be used for non-urgent bulk
//          xfers, which are not time relevant)
//      100 - highest priority (should be used only for really
//          critically urgent messages, which need to be delivered
//          asap)
//
//      NOTE: in legacy ICCom implementation is not used.
//
// @finalized true if the message is done and ready to be delivered
//      to the consumer. False if message is under construction.
// @uncommitted_length the size of uncommitted part of the message data
//      this value indicates how many bytes in in message data were
//      added with the last package. If package parsing fails at some
//      later point, then the whole package will be resent and
//      received again, thus as long we have no packets IDs in
//      protocol right now, we need to revert all applied changes
//      from failed package to maintain data integrity.
struct iccom_message {
	struct list_head list_anchor;

	char *data;
	size_t length;
	unsigned int channel;
	unsigned int id;
	unsigned int priority;
	bool finalized;

	size_t uncommitted_length;
};

// Describes the single data package. The data package is a data,
// which is sent/received within single data xfer of underlying
// communication layer. So package data is identical with xfer data.
//
// @list_anchor the binding to the list
// @data the raw xfer data to be sent
// @size the size of the xfer for underlying layer (in bytes):
//      total number of bytes in the package
// @owns_data if true, then the data pointed by xfer_data is owned
//      by the package and must be freed upon package destruction.
//
// NOTE: for now we will use the following package configuration:
//      SALT documentation, 20 November 2018, 1.4.2 Transmission
//      blocks: Table 8:
//          * 2 byte message length,
//          * 1 byte package sequential ID
//          * PAYLOAD (size depends on package size)
//          * 2 byte SRC
//      This is done for backward compatibility with the previous
//      implementation.
//
struct iccom_package {
	struct list_head list_anchor;

	unsigned char *data;
	size_t size;

	bool owns_data;
};

// Packet header descriptor.
// See SALT documentation, 20 November 2018, 1.4.4 Payload data
//     organization, blocks: Table 11, Table 13.
typedef struct {
	uint16_t payload: 16;
	uint8_t lun: 8;
	uint8_t cid: 7;
	uint8_t complete: 1;
} iccom_package_header;

// The structure describes the ICCom packet
// @payload pointer to the first payload byte
//      NOTE: NULL <=> invalid packet
//      NOTE: never owns the payload data.
// @payload_length the size of the packet payload (in bytes).
//      NOTE: 0 <=> invalid packet
// @channel the channel to which the packet is attached.
// @finalizing if true, then the packet contains the last
//      chunk of the corresponding message.
//
// See SALT documentation, 20 November 2018
//          , 1.4.4 Payload data organization
//      blocks: Table 11, Table 13.
struct iccom_packet {
	void *payload;
	size_t payload_length;
	unsigned int channel;
	bool finalizing;
};

// The channel record of iccom_message_storage
// Contains the link to the next channel and link to
// messages of current channel.
//
// @channel_anchor the anchor of channels list
// @channel the channel value
// @messages the head of the channel messages list
//      NOTE: the incoming messages are always added to the
//          end of the list (to its tail).
// @current_last_message_id the id of the latest message in
//      the channel (the value is not relevant if there is
//      no messages in the channel).
//      TODO: this field should be removed as message id is
//          added to the packet structure.
// @consumer_callback_data {ANY} data to provide to the consumer
//      in the @message_ready_callback
// @message_ready_callback {NULL || valid callback pointer}
//      points to the consumer callback, which is called every
//      time when message gets ready in the channel.
//
//      This callback is called from a separate thread, however
//      all callbacks are executed in the same thread, so long
//      callback processing will lead to blocking of other consumers.
//
//      TODO: avoid this cross-consumer blocking dependency
//
//      RETURNS:
//          true: then ownership of message data (@msg_data) is
//              transferred to the consumer;
//          false: then message data ownership remains in ICCom,
//              and is immediately discarded after callback invocation.
struct iccom_message_storage_channel
{
	struct list_head channel_anchor;
	unsigned int channel;

	struct list_head messages;

	unsigned int current_last_message_id;

	void *consumer_callback_data;
	iccom_msg_ready_callback_ptr_t message_ready_callback;
};

// Describes the messages storage. Intended to be used to
// store the received ICCom messages while they are under construction
// or were constructed but not yet fetched by consumer.
//
// The exact implementation of the struct is defined by speed and
// memory requirements and may vary, but the interface methods
// (iccom_message_storage_*) are intended to persist.
//
// @channels_list the list of channel records of the storage.
// @lock the mutex to protect the storage from data-races.
//      NOTE: we will try to lock mutex only for operations
//          directly on the storage, while leaving the message
//          data writing/copying unlocked.
//          (this implies that consumer guarantees that no concurrent
//          calls to the same channel will happen)
// @message_ready_global_callback {NULL || valid callback pointer}
//      points to the consumer global callback, which is called every
//      time when channel doesn't have a dedicated channel callback
//      defined. If it is NULL, then will not be invoked.
// @global_consumer_data {any} this value is passed to the
//      @message_ready_global_callback.
// @uncommitted_finalized_count the number of finalized messages since
//      last commit.
struct iccom_message_storage
{
	struct list_head channels_list;
	struct mutex lock;

	iccom_msg_ready_callback_ptr_t message_ready_global_callback;
	void *global_consumer_data;

	int uncommitted_finalized_count;
};

// Iccom device statistics
//
// @packages_bad_data_received incremented every time we get
//      a package with a broken data (like wrong length, wrong
//      CRC32 sum, etc.).
// @packages_duplicated_received incremented every time we
//      get a duplicated package.
// @packages_parsing_failed incremented every time we fail
//      to parse the package data into correct packets.
//
// NOTE: statistics is not guaranteed to be percise or even
//      selfconsistent. This data is mainly for debugging,
//      general picture monitoring. Don't use these values
//      for non-statistical/monitoring purposes. This is due
//      to absence of correct sync in statistics operations
//      which otherwise will introduce too big overhead.
struct iccom_dev_statistics {
	unsigned long long transport_layer_xfers_done_count;
	unsigned long long raw_bytes_xfered_via_transport_layer;
	unsigned long long packages_xfered;
	unsigned long long packages_sent_ok;
	unsigned long long packages_received_ok;
	unsigned long long packages_bad_data_received;
	unsigned long long packages_duplicated_received;
	unsigned long long packages_parsing_failed;
	unsigned long long packets_received_ok;
	unsigned long long messages_received_ok;
	unsigned long packages_in_tx_queue;
	unsigned long long total_consumers_bytes_received_ok;
	unsigned long messages_ready_in_storage;

// TODO:
//	unsigned long long packets_sent;
//	unsigned long long messages_sent;
//	unsigned long long total_consumers_bytes_sent;
};

// Keeps the error history record
// @err_num keeps the error number which the record belongs to
// @total_count the total count of the error happened since last
//      ICCom start
// @in_curr_report_interval_count the number of errors of @err_num
//      type happened within current report interval.
// @last_report_time_msec the msec time when the error type was
//      last reported. If new error comes earlier than
//      @last_report_time_msec + ICCOM_MIN_ERR_REPORT_INTERVAL_MSEC
//      then it is only put into statistics but not reported (will be
//      reported as new error of this type occurred after silence time)
//      NOTE: or if error rate threshold is reached
// @last_occurrence_time_msec the time interval since last event of the
//      error in mseconds.
// @exp_avg_interval_msec the exponentially weightened average interval
// 	between error events in mseconds.
// @err_msg the error message to be sent to kernel message buffer
// @last_reported is set to true, when the last error was reported to
//      user.
// @err_per_sec_threshold sets the error rate starting from which the
// 	error is reported as error (not as warning or info). This
// 	will be used to identify the real issues like full stall of
// 	communication among occasional errors which always might happen
// 	on HW communication line.
struct iccom_error_rec {
	unsigned char err_num;
	unsigned int total_count;
	unsigned int unreported_count;
	unsigned long last_report_time_msec;
	unsigned long last_occurrence_time_msec;
	unsigned long exp_avg_interval_msec;
	const char *err_msg;
	bool last_reported;
	unsigned int err_per_sec_threshold;
};

// Describes the internal ICCom data
// @iccom pointer to corresponding iccom_dev structure.
// @tx_data_packages_head the list of data packages (struct iccom_package)
//      to send to the other side. Head.next points to earliest-came
//      from consumer message.
//
//      As long as messages have no IDs and message parts have no
//      sequential ID in legacy (rev.1.0) protocol
//      implementationm, then we are not able to shuffle
//      the messages parts for xfer for the same channel. If
//      message started to be xfered in given channel, it should
//      be xfered till no its data left, sequentially.
//
//      The list doesn't contain the ack packages, only data packages.
//
//      NOTE: there is always at least one package in the list
//          after any queue manipulation ends.
//
//      NOTE: all packages in queue are always finalized
//          between queue manipulations
//      NOTE: new package is added only when we need it (either
//          no packages to prepare for xfer, or previous
//          package in TX queue ran out of space but we still
//          have data to write to TX)
//
//      TX queue manipulation routines:
//          __iccom_queue_*
//
// @tx_queue_lock mutex to protect the TX packages queue from data
//      races.
// @ack_val const by usage. Keeps the ACK value, which is to be sent to
//      the other side when ACK.
// @nack_val const by usage. Keeps the NACK value, which is to be sent to
//      the other side when NACK.
// @xfer the currently going xfer, it only points to the data but never
//      owns it.
// @data_xfer_stage according to the protocol, data and ack packages are
//      interleave, so this field indicates if we are in the data xfer
//      cycle now or in ack xfer cycle. If the filed is true, then we
//      are now in data xfer stage, and thus
//          * if underlying layer is not busy, then we are free to start
//            data package xfer,
//          * if underlying layer is busy, then upon xfer finished we
//            need to send the ack package.
//      If the field is false (implies underlying layer is busy):
//          * then upon xfer finished we may start next data package xfer.
// @next_tx_message_id keeps the next outgoing message id. Wraps around.
// @last_rx_package_id the sequence ID of the last package we have
//      received from the other side. If we receive two packages
//      with the same sequence ID, than we will drop all but one of the
//      packages with the same sequence ID.
// @rx_messages the incoming messages storage. Stores completed incoming
//      messages as well as under construction incoming messages.
// @work_queue pointer to personal ICCom dedicated work-queue to handle
//      communication jobs. It is used mainly for stability consderations
//      cause publicitly available work-queue can potentially be blocked
//      by other running/pending tasks. So to stay on a safe side we
//      will allocate our own single-threaded workqueue for our purposes.
//      NOTE: used only when ICCOM_WORKQUEUE_MODE equals ICCOM_WQ_PRIVATE
// @consumer_delivery_work the kworker which is responsible for
//      notification and delivery to the consumer finished incoming
//      messages.
// @closing true only when iccom device is going to be shutdown.
// @statistics the integral operational information about ICCom device
//      instance.
// @errors tracks the errors by type, and allows the flooding error
//      reporting protection
// @proc_root the root iccom directory in the proc file system
//      this directory is now aiming to provide statistical
//      information on ICCom but later might be used to set some
//      ICCom parameters dynamically.
// @statistics_ops ICCom statistics device operations (to read out
//      ICCom statistics info to user space)
// @statistics_file the file in proc fs which provides the ICCom
//      statistics to user space.
struct iccom_dev_private {
	struct iccom_dev *iccom;

	struct list_head tx_data_packages_head;
	struct mutex tx_queue_lock;

	unsigned char ack_val;
	unsigned char nack_val;

	// never owns the data pointed to
	struct full_duplex_xfer xfer;

	bool data_xfer_stage;

	int next_tx_package_id;
	int last_rx_package_id;

	struct iccom_message_storage rx_messages;

#if ICCOM_WORKQUEUE_MODE_MATCH(PRIVATE)
	struct workqueue_struct *work_queue;
#endif
	struct work_struct consumer_delivery_work;

	bool closing;

	struct iccom_dev_statistics statistics;

	struct iccom_error_rec errors[ICCOM_ERROR_TYPES_COUNT];

	struct proc_dir_entry *proc_root;

	struct file_operations statistics_ops;
	struct proc_dir_entry *statistics_file;
};

/* ------------------------ GLOBAL VARIABLES ----------------------------*/

// Serves to speed up the CRC32 calculation using the precomputed values.
uint32_t iccom_crc32_lookup_tbl[256];

static const char ICCOM_ERROR_S_NOMEM[] = "no memory available";
static const char ICCOM_ERROR_S_TRANSPORT[]
	= "Xfer failed on transport layer. Restarting frame.";

/* ------------------------ FORWARD DECLARATIONS ------------------------*/

#ifdef ICCOM_DEBUG
static int __iccom_msg_storage_printout_channel(
		struct iccom_message_storage_channel *channel_rec
		, int max_printout_count);
static int __iccom_msg_storage_printout(
		struct iccom_message_storage *storage
		, int max_printout_count
		, int channel);
#endif

/* ----------------------------- UTILS ----------------------------------*/

// Generates the CRC32 lookup table (on Little Endian data)
// (top bit is at pos 0).
//
// SEE ALSO:
//   https://en.wikipedia.org/wiki/Cyclic_redundancy_check
//   https://www.kernel.org/doc/Documentation/crc32.txt
//   https://barrgroup.com/Embedded-Systems/How-To/CRC-Calculation-C-Code
static void __iccom_crc32_gen_lookup_table(void)
{
	uint32_t crc;

	// CRC32 parameters
	const uint32_t POLYNOMIAL = 0xEDB88320;
	const uint32_t TOP_BIT = 0x00000001;
	const uint32_t DIVIDENT_SIZE_BITS = 8;

	for (uint32_t i = 0; i < ARRAY_SIZE(iccom_crc32_lookup_tbl); i++) {
		crc = i;
		for (int j = 0; j < DIVIDENT_SIZE_BITS; j++) {
			crc = (crc & TOP_BIT) ? ((crc >> 1) ^ POLYNOMIAL)
					      : (crc >> 1);
		}
		iccom_crc32_lookup_tbl[i] = crc;
	}
}

// Computes CRC32 (on Little Endian data) (top bit is at pos 0).
// @data {valid data ptr if size > 0, otherwise any}
// @size_bytes {size of data in bytes && >= 0}
//
// The target formula:
// crc = (crc >> 8) ^ little_endian_table[data[i] ^ (8 right bits of crc)]
//
// SEE ALSO: https://en.wikipedia.org/wiki/Cyclic_redundancy_check
//           https://www.kernel.org/doc/Documentation/crc32.txt
static inline uint32_t __iccom_compute_crc32(
		uint8_t *data, size_t size_bytes)
{
	const uint8_t BITMASK = 0xFF;
	const uint8_t BITMASK_SIZE = 8;

	uint32_t crc = 0xFFFFFFFF;
	uint8_t *end_ptr = data + size_bytes;

	// byte-wise computation, MSB first
	for (; data != end_ptr; ++data) {
		uint8_t lookup_idx = (uint8_t)((crc ^ (*data)) & BITMASK);
		crc = (crc >> BITMASK_SIZE) ^ iccom_crc32_lookup_tbl[lookup_idx];
	}

	return ~crc;
}

/* --------------------- RAW PACKAGE MANIPULATION -----------------------*/

// Helper. Gets the size of package overall payload space even it is
// already occupied with some payload.
// See @iccom_package description.
static inline size_t __iccom_package_payload_room_size(
		struct iccom_package *package)
{
	return package->size
		- ICCOM_PACKAGE_PAYLOAD_DATA_LENGTH_FIELD_SIZE_BYTES
		- ICCOM_PACKAGE_ID_FIELD_SIZE_BYTES
		- ICCOM_PACKAGE_CRC_FIELD_SIZE_BYTES;
}

// Helper. Sets the package payload length. See @iccom_package description.
static inline void __iccom_package_set_payload_size(
		struct iccom_package *package, size_t length)
{
	*((__be16*)package->data) = __cpu_to_be16((uint16_t)length);
}

// Helper. Gets the package payload length. See @iccom_package description.
static inline size_t __iccom_package_payload_size(
		struct iccom_package *package, bool *ok__out)
{
	size_t declared_size = (size_t)__be16_to_cpu(*((__be16*)package->data));
	size_t max_possible = __iccom_package_payload_room_size(package);
	if (declared_size <= max_possible) {
		if (!IS_ERR_OR_NULL(ok__out)) {
			*ok__out = true;
		}
		return declared_size;
	}
	if (!IS_ERR_OR_NULL(ok__out)) {
		*ok__out = false;
	}
	return 0;
}

// Helper. Gets if the package is empty.
static inline size_t __iccom_package_is_empty(
		struct iccom_package *package, bool *ok__out)
{
	return __iccom_package_payload_size(package, ok__out) == 0;
}

// Helper. Returns the pointer to the first byte of the payload.
static inline void *__iccom_package_payload_start_addr(
		struct iccom_package *package)
{
	return package->data
		+ ICCOM_PACKAGE_PAYLOAD_DATA_LENGTH_FIELD_SIZE_BYTES
		+ ICCOM_PACKAGE_ID_FIELD_SIZE_BYTES;
}

// Helper. Gets the size of package free space for payload (in bytes).
// See @iccom_package description.
static inline size_t __iccom_package_get_payload_free_space(
		struct iccom_package *package, bool *ok__out)
{
	return __iccom_package_payload_room_size(package)
	       - __iccom_package_payload_size(package, ok__out);
}

// Helper. Sets the package ID. See @iccom_package description.
static inline void __iccom_package_set_id(
		struct iccom_package *package, int id)
{
	*((uint8_t*)package->data
		+ ICCOM_PACKAGE_PAYLOAD_DATA_LENGTH_FIELD_SIZE_BYTES)
			= (uint8_t)id;
}

// Helper. Gets the package ID. See @iccom_package description.
static inline int __iccom_package_get_id(
		struct iccom_package *package)
{
	return (int)(*((uint8_t*)package->data
			+ ICCOM_PACKAGE_PAYLOAD_DATA_LENGTH_FIELD_SIZE_BYTES));
}

// Helper. Sets the package SRC. See @iccom_package description.
static inline void __iccom_package_set_src(
		struct iccom_package *package, unsigned int src)
{
	int src_offset = package->size - ICCOM_PACKAGE_CRC_FIELD_SIZE_BYTES;
	*((uint32_t *)((uint8_t *)package->data + src_offset)) = (uint32_t)src;
}

// Helper. Gets the package SRC. See @iccom_package description.
static inline unsigned int __iccom_package_get_src(
		struct iccom_package *package)
{
	int src_offset = package->size - ICCOM_PACKAGE_CRC_FIELD_SIZE_BYTES;
	return *((uint32_t *)((uint8_t *)package->data + src_offset));
}

// Helper. Returns the address of the beginning of the package payload
// free space.
//
// NOTE: if no free spage returns NULL
static inline void * __iccom_package_get_free_space_start_addr(
		struct iccom_package *package, bool *ok__out)
{
	size_t free_length = __iccom_package_get_payload_free_space(
						   package, ok__out);
	if (!free_length) {
		return NULL;
	}
	return ((uint8_t *)package->data + package->size
			    - ICCOM_PACKAGE_CRC_FIELD_SIZE_BYTES)
	       - free_length;
}

// Helper. Fills package unused payload area with symbol.
// See @iccom_package description.
//
// RETURNS:
//      number of filled bytes
static unsigned int __iccom_package_fill_unused_payload(
		struct iccom_package *package, uint8_t symbol)
{
	// See @iccom_package description.

	size_t free_length = __iccom_package_get_payload_free_space(
						      package, NULL);

	if (free_length == 0) {
		return free_length;
	}

	memset(__iccom_package_get_free_space_start_addr(package, NULL)
	       , symbol, free_length);

	return free_length;
}

// Helper. Verifies that all free payload bytes set to given symbol.
// @package the package with at least checked payload size not exceeding
//      the package size
//
// RETURNS:
//      true: if all is OK (all unused payload bytes are set to
//          given symbol)
//      false: else
bool __iccom_package_check_unused_payload(
		struct iccom_package *package, uint8_t symbol)
{
	// See @iccom_package description.
	bool ok = false;
	const size_t free_length = __iccom_package_get_payload_free_space(
							     package, &ok);
	if (!ok) {
		return false;
	}
	uint8_t *start = __iccom_package_get_free_space_start_addr(package
								   , &ok);
	if (!ok) {
		return false;
	}
	int32_t val32 = symbol | symbol << 8 | symbol << 16 | symbol << 24;
	for (int i = 0; i < free_length / 4; i++) {
		if (*((int32_t*)start) != val32) {
			return false;
		}
		start += 4;
	}
	for (int j = 0; j < free_length % 4; j++) {
		if (*(start) != symbol) {
			return false;
		}
		start++;
	}

	return true;
}

// Helper. Returns the pointer to an iccom_package structure
// given by its list anchor pointer.
static inline struct iccom_package *__iccom_get_package_from_list_anchor(
		struct list_head *anchor)
{
	const int offset = offsetof(struct iccom_package, list_anchor);
	return (struct iccom_package *)((char*)anchor - offset);
}

// Helper. Returns the pointer to first package in TX queue.
// NOTE: the first package still could be unfinished.
//
// LOCKING: storage should be locked before this call
//
// RETURNS:
//      pointer to the first package in TX queue if one;
//      NULL if no packages in TX queue;
static inline struct iccom_package *__iccom_get_first_tx_package(
		struct iccom_dev *iccom)
{
	if (list_empty(&iccom->p->tx_data_packages_head)) {
		return NULL;
	}
	return __iccom_get_package_from_list_anchor(
			iccom->p->tx_data_packages_head.next);
}

// Helper. No locking.
//
// RETURNS:
//      {valid ptr} : the pointer to last (latest came) package in
//          TX queue.
//      {NULL} : if TX queue is empty
static inline struct iccom_package *__iccom_get_last_tx_package(
		struct iccom_dev *iccom)
{
	if (list_empty(&iccom->p->tx_data_packages_head)) {
		return NULL;
	}
	return __iccom_get_package_from_list_anchor(
			iccom->p->tx_data_packages_head.prev);
}

/* --------------------- PACKAGE MANIPULATION ---------------------------*/

// Computes CRC32 on the package.
// @package {valid package ptr}
// RETURNS:
//      computed CRC32 value
static unsigned int __iccom_package_compute_src(
	struct iccom_package *package)
{
	return __iccom_compute_crc32(package->data
		, package->size - ICCOM_PACKAGE_CRC_FIELD_SIZE_BYTES);
}

// Helper. Inits data structures of the package to the initial
// empty unfinalized package state.
//
// After this operation one may add data to the package.
//
// @package the allocated package struct to initialize
// @package_size the size of the package in bytes
//
// RETURNS:
//      0 on success
//      < 0 - the negative error code
static int __iccom_package_init(struct iccom_package *package
		, size_t package_size_bytes)
{
	package->size = package_size_bytes;
	package->owns_data = true;
	package->data = kmalloc(package->size, GFP_KERNEL);
	if (!package->data) {
		iccom_err("no memory");
		package->size = 0;
		return -ENOMEM;
	}

	__iccom_package_set_payload_size(package, 0);

	INIT_LIST_HEAD(&package->list_anchor);
	return 0;
}

// Helper. Frees the package data, allocated for @package structure
// if package owns the data, and the package itself.
//
// @package pointer to iccom_package structure allocated on heap.
//
// LOCKING: storage should be locked before this call
static void __iccom_package_free(struct iccom_package *package)
{
	if (package->owns_data) {
		kfree(package->data);
	}
	package->data = NULL;
	list_del(&package->list_anchor);
	kfree(package);
}

// Helper. Finishes the creation of the package.
//      * fills up unused payload data with
//        ICCOM_PACKAGE_EMPTY_PAYLOAD_VALUE
//      * sets correct CRC sum.
// @package {valid ptr to package}
//
// After this operation package is correct.
// After this call the package is ready to be sent.
static void __iccom_package_finalize(struct iccom_package *package)
{
	__iccom_package_fill_unused_payload(package
			, ICCOM_PACKAGE_EMPTY_PAYLOAD_VALUE);
	__iccom_package_set_src(package, __iccom_package_compute_src(package));
}

// Helper. Clears the package to make it contain no payload.
// @package {valid ptr to package}
//
// After this operation package is correct and empty.
// After this call the package is ready to be sent.
static void __iccom_package_make_empty(struct iccom_package *package)
{
	__iccom_package_set_payload_size(package, 0);
	__iccom_package_finalize(package);
}

#ifdef ICCOM_DEBUG
static void iccom_dbg_printout_package(struct iccom_package *pkg)
{
	ICCOM_CHECK_PTR(pkg, return);

	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL
		       , "========= PACKAGE:");
	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL
		       , "ptr: %px\tdata ptr: %px\tdata size: %zu"
		       , pkg, pkg->data, pkg->size);
	print_hex_dump(KERN_DEBUG, ICCOM_LOG_PREFIX"PKG data: ", 0, 16
		       , 1, pkg->data, pkg->size, true);
	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL, "= Decoded info: =");
	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL
		       , "PL size: %zu\tPL free: %zu\tid: %d\tCRC: %u"
		       , __iccom_package_payload_size(pkg, NULL)
		       , __iccom_package_get_payload_free_space(pkg, NULL)
		       , __iccom_package_get_id(pkg)
		       , __iccom_package_get_src(pkg));
}

// @max_printout_count {>=-1}, maximum number of packaged to print total,
//      -1 means "unlimited", 0 means "do not print"
static void iccom_dbg_printout_tx_queue(struct iccom_dev *iccom
		, int max_printout_count)
{
	ICCOM_CHECK_DEVICE("", return);
	ICCOM_CHECK_DEVICE_PRIVATE("", return);

	if (!max_printout_count) {
		return;
	}

	int printed = 0;
	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL
		   , "======= The TX packages queue: BEGIN");
	struct iccom_package *pkg;
	list_for_each_entry(pkg, &iccom->p->tx_data_packages_head
			    , list_anchor) {
		if (max_printout_count > 0 && printed >= max_printout_count) {
			iccom_warning_raw("PACKAGES QUEUE PRINTOUT CUTOFF");
			break;
		}
		iccom_dbg_printout_package(pkg);
		printed++;
	}
	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL
		   , "======= The TX packages queue: END");
}

// TODO: extract to independent source
// TODO: fix print to contain log prefix
void iccom_dbg_printout_xfer(const struct full_duplex_xfer *const xfer)
{
	if (IS_ERR(xfer)) {
		printk("xfer ptr BROKEN: %px\n", xfer);
		return;
	} else if (!xfer) {
		printk("xfer ptr NULL\n");
		return;
	}
	printk("Xfer ptr: %px\n", xfer);
	printk("Xfer size: %u\n", xfer->size_bytes);
	if (IS_ERR(xfer->data_tx)) {
		printk("Xfer TX data ptr: BROKEN: %px\n", xfer->data_tx);
	} else if (xfer->data_tx) {
		printk("Xfer TX data ptr: %px\n", xfer->data_tx);
		print_hex_dump(KERN_DEBUG, "TX data: ", 0, 16
			    , 1, xfer->data_tx, xfer->size_bytes, true);
	} else {
		printk("Xfer TX data ptr: NULL\n");
	}
	if (IS_ERR(xfer->data_rx_buf)) {
		printk("Xfer RX data ptr: BROKEN: %px\n", xfer->data_rx_buf);
	} else if (xfer->data_rx_buf) {
		printk("Xfer RX data ptr: %px\n", xfer->data_rx_buf);
		print_hex_dump(KERN_DEBUG, "RX data: ", 0, 16
			    , 1, xfer->data_rx_buf, xfer->size_bytes
			    , true);
	} else {
		printk("Xfer RX data ptr: NULL\n");
	}
}

const char state_finalized[] = "finalized";
const char state_under_construction[] = "under construction";
void iccom_dbg_printout_message(const struct iccom_message *const msg)
{
	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL, "-- message --");
	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL
		       , "ch: %u\tid: %u\tpriority: %u\tlen: %u"
			 "\tuncommitted len: %u\t state: %s"
		       , msg->channel
		       , msg->id, msg->priority, msg->length
		       , msg->uncommitted_length
		       , msg->finalized ? state_finalized
					: state_under_construction);
	if (IS_ERR(msg->data)) {
		iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL
			       , "data: broken: %px", msg->data);
	} else if (!msg->data) {
		iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL, "data: NULL");
	} else {
		print_hex_dump(KERN_DEBUG, "data: ", 0, 16
			       , 1, msg->data, msg->length, true);
	}
}

#else /* ICCOM_DEBUG */

// remove debug methods
#define iccom_dbg_printout_package(pkg)
#define iccom_dbg_printout_tx_queue(iccom, max_printout_count)
#define iccom_dbg_printout_xfer(xfer)
#define iccom_dbg_printout_message(msg)

#endif /* ICCOM_DEBUG */

/* --------------------- PACKETS MANIPULATION ---------------------------*/

// Returns packet size for given payload size (all in bytes).
//
// See SALT documentation, 20 November 2018
//          , 1.4.4 Payload data organization
//      blocks: Table 11, Table 13.
static inline size_t iccom_packet_packet_size_bytes(
		const size_t payload_size)
{
	return ICCOM_PACKET_HEADER_SIZE_BYTES + payload_size;
}

// Returns minimal packet size in bytes.
//
// See SALT documentation, 20 November 2018
//          , 1.4.4 Payload data organization
//      blocks: Table 11, Table 13.
static inline size_t iccom_packet_min_packet_size_bytes(void)
{
	return iccom_packet_packet_size_bytes(1);
}

// Returns the packet payload begin address
// @package_begin the address of the first packet byte
//
// See SALT documentation, 20 November 2018
//          , 1.4.4 Payload data organization
//      blocks: Table 11, Table 13.
static inline void *iccom_packet_payload_begin_addr(
		void *package_begin)
{
	return package_begin + ICCOM_PACKET_HEADER_SIZE_BYTES;
}

// Conversion to legacy format of LUN and CID
// @channel channel (if channel is bigger than
//      supported, the higher bits are truncated.
static inline uint8_t iccom_packet_channel_lun(
		const unsigned int channel)
{
	return (uint8_t)(((uint32_t)channel >> 7) & 0x000000FF);
}

// Conversion to legacy format of LUN and CID
// @channel channel
static inline uint8_t iccom_packet_channel_sid(
		const unsigned int channel)
{
	return (uint8_t)(((uint32_t)channel) & 0x0000007F);
}

// Conversion from legacy format of LUN and CID
// @lun the LUN value
// @cid the CID value, if bigger than allowed - truncated
static inline unsigned int iccom_packet_luncid_channel(
		const uint8_t lun, const uint8_t cid)
{
	return (unsigned int)((((uint32_t)lun) << 7)
			      | (((uint32_t)cid) & 0x0000007F));
}

// Helper. Writes the packet header into the destination given by @target.
//
// @payload_size_bytes valid payload value
// @channel valid channel value
// @message_complete if this packet finalizes the message
// @target should have at least ICCOM_PACKET_HEADER_SIZE_BYTES
//      bytes available.
//
// See SALT documentation, 20 November 2018
//          , 1.4.4 Payload data organization
//      blocks: Table 11, Table 13.
//
// RETURNS:
//      number of bytes written to target
inline static size_t iccom_packet_write_header(
		const size_t payload_size_bytes
		, const unsigned int channel
		, const bool message_complete
		, void * const target)
{
	((iccom_package_header*)target)->payload
			= __cpu_to_be16((uint16_t)payload_size_bytes);
	((iccom_package_header*)target)->lun
			= iccom_packet_channel_lun(channel);
	((iccom_package_header*)target)->complete
			= message_complete ? 1 : 0;
	((iccom_package_header*)target)->cid
			= iccom_packet_channel_sid(channel);

	return ICCOM_PACKET_HEADER_SIZE_BYTES;
}

// Fills up the packet structure from given raw byte package data.
//
// @start_from {valid kernel pointer} the pointer to the first byte
//      of the packet (including header)
// @max_bytes_available {>=0} the maximum possible size of the packet
//      (usually restricted by corresponsding package payload area).
// @packet__out {valid pointer} pointer to the iccom_packet structure
//      to write the parsed packet data in.
//
// See SALT documentation, 20 November 2018
//          , 1.4.4 Payload data organization
//      blocks: Table 11, Table 13.
//
// RETURNS:
//      0 on success
//      <0 - negated error valus if failed
static int __iccom_packet_parse_into_struct(
		void *start_from
		, const size_t max_bytes_available
		, struct iccom_packet *packet__out)
{
#ifdef ICCOM_DEBUG
	if (IS_ERR_OR_NULL(start_from)) {
		iccom_err("Broken start_from pointer");
		return -EINVAL;
	}
	if (IS_ERR_OR_NULL(packet__out)) {
		iccom_err("Broken packet__out pointer");
		return -EINVAL;
	}
#endif
	iccom_package_header *src = (iccom_package_header*)start_from;

	if (max_bytes_available < iccom_packet_min_packet_size_bytes()) {
		goto invalidate_packet;
	}

	packet__out->payload_length = (size_t)(__be16_to_cpu(src->payload));
	if (iccom_packet_packet_size_bytes(packet__out->payload_length)
			> max_bytes_available) {
		goto invalidate_packet;
	}
	packet__out->channel = iccom_packet_luncid_channel(src->lun, src->cid);
	packet__out->finalizing = (bool)src->complete;
	packet__out->payload = iccom_packet_payload_begin_addr(start_from);

	return 0;

invalidate_packet:
	packet__out->payload = NULL;
	packet__out->payload_length = 0;
	return -EINVAL;
}

// Adds the maximum possible amount of bytes from message to the package.
// Wraps message data into the packet data structure and then adds
// the packet to the package payload area.
//
// NOTE: doesn't finalize the package, as long as more info might be
//      added to the package later.
//
// @package {valid ptr || NULL} the package to try to add the data to
//      if NULL then function does nothing and correspondingly returns
//      0.
// @packet_payload the consumer payload to put into the packet which then
//      will be added to the package. This data is wrapped into packet
//      and then the packet is added to the package payload room.
// @payload_size_bytes the lengthe of @packet_payload in bytes.
// @channel the channel to which the packet attached
//
// RETURNS:
//      the number of consumer payload bytes which were added to the
//      package.
//      0 means that no more pakets can be added to the package. So
//      the package is ready to be sent.
static size_t iccom_package_add_packet(struct iccom_package *package
		, char *packet_payload, const size_t payload_size_bytes
		, const unsigned int channel)
{
	if (IS_ERR_OR_NULL(package)) {
		return 0;
	}

	size_t package_free_space_bytes
		= __iccom_package_get_payload_free_space(package, NULL);

	if (package_free_space_bytes <= ICCOM_PACKET_HEADER_SIZE_BYTES) {
		return 0;
	}

	// size of payload to be written to the packet
	size_t payload_write_size_bytes = package_free_space_bytes
					       - ICCOM_PACKET_HEADER_SIZE_BYTES;
	if (payload_write_size_bytes > payload_size_bytes) {
		payload_write_size_bytes = payload_size_bytes;
	}

	size_t bytes_written_to_package = 0;
	uint8_t *start_ptr = __iccom_package_get_free_space_start_addr(package
								       , NULL);

	bytes_written_to_package += iccom_packet_write_header(
					    payload_write_size_bytes
					    , channel
					    , payload_write_size_bytes
						    == payload_size_bytes
					    , start_ptr);
	memcpy(start_ptr + bytes_written_to_package, packet_payload
	       , payload_write_size_bytes);

	bytes_written_to_package += payload_write_size_bytes;

	size_t new_length = __iccom_package_payload_size(package, NULL)
			    + bytes_written_to_package;
	__iccom_package_set_payload_size(package, new_length);

	return payload_write_size_bytes;
}

/* ------------------ MESSAGES MANIPULATION -----------------------------*/

// Helper. Initializes new message struct.
// @msg {valid msg struct ptr}
static inline void __iccom_message_init(struct iccom_message __kernel *msg)
{
	memset(msg, 0, sizeof(struct iccom_message));
	INIT_LIST_HEAD(&msg->list_anchor);
}

// Helper. Frees the data, allocated by message. The message struct
// itself is managed by the caller. If message is in the list,
// then it is removed from the list. Frees the message itself also.
//
// @msg message allocated on heap
//
// LOCKING: protection of the list the message may be in is the
//      responsibility of the caller.
static void __iccom_message_free(struct iccom_message *msg)
{
	if (IS_ERR_OR_NULL(msg)) {
		return;
	}
	list_del(&(msg->list_anchor));
	if (!IS_ERR_OR_NULL(msg->data)) {
		kfree(msg->data);
	}
	kfree(msg);
}

// Helper. Returns if the message is ready
static inline bool __iccom_message_is_ready(struct iccom_message *msg)
{
	return msg->finalized && msg->uncommitted_length == 0;
}

/* ---------------- MESSAGES STORE PRIVATE SECTION -----------------*/

// Helper. Returns channel from channel list anchor. No checks.
static inline struct iccom_message_storage_channel *
__iccom_msg_storage_anchor2channel(struct list_head *anchor)
{
	return container_of(anchor
			    , struct iccom_message_storage_channel
			    , channel_anchor);
}

// Helper. Returns next channel in the channels list or NULL
// if next is head.
static inline struct iccom_message_storage_channel *
__iccom_msg_storage_next_channel(
		struct iccom_message_storage_channel * const ch
		, struct list_head *const head)
{
	if (ch->channel_anchor.next == head) {
		return NULL;
	}
	return container_of(ch->channel_anchor.next
			    , struct iccom_message_storage_channel
			    , channel_anchor);
}

// Helper. Returns previous channel in the channels list. No checks.
static inline struct iccom_message_storage_channel *
__iccom_msg_storage_prev_channel(
		struct iccom_message_storage_channel * const ch
		, struct list_head *const head)
{
	if (ch->channel_anchor.prev == head) {
		return NULL;
	}
	return container_of(ch->channel_anchor.prev
			    , struct iccom_message_storage_channel
			    , channel_anchor);
}

// Helper. Tries to find a record which corresponds to a given channel.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to find
//
// LOCKING: storage should be locked before calling this function
//
// RETURNS:
//      !NULL pointer to storage channel record - if found
//      NULL - if not
struct iccom_message_storage_channel *__iccom_msg_storage_find_channel(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
	struct iccom_message_storage_channel *channel_rec;
	list_for_each_entry(channel_rec, &storage->channels_list
			    , channel_anchor) {
		if (channel_rec->channel == channel) {
			return channel_rec;
		}
	}
	return NULL;
}

// Helper. Tries to find a message in given channel record given
// by message id.
//
// @channel_rec {valid ptr || NULL}
//      valid: points to the channel to search the message in
//      NULL: then function simply returns NULL.
// @msg_id the target id of the message to search for
//
// LOCKING: storage should be locked before calling this function
//
// RETURNS:
//      !NULL pointer to message - if the message is found
//      NULL pointer - if message was not found or nowhere to search
struct iccom_message *__iccom_msg_storage_find_message_in_channel(
		struct iccom_message_storage_channel *channel_rec
		, unsigned int msg_id)
{
	if (!channel_rec) {
		return NULL;
	}

	struct iccom_message *msg;
	list_for_each_entry(msg, &channel_rec->messages
			    , list_anchor) {
		if (msg->id == msg_id) {
			return msg;
		}
	}

	return NULL;
}

// Helper. Adds a new channel to the storage. If channel exists, returns
// pointer to existing struct.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {valid channel number} the channel number to add/retrieve
//
// LOCKING: storage should be locked before calling this function
//
// RETURNS:
//      Pointer to existing or newly created channel:
//          if everything is OK.
//      NULL: if allocation of new channel failed.
static struct iccom_message_storage_channel *
__iccom_msg_storage_add_channel(struct iccom_message_storage *storage
				, unsigned int channel)
{
	struct iccom_message_storage_channel * channel_rec
		= __iccom_msg_storage_find_channel(storage, channel);
	if (channel_rec) {
		return channel_rec;
	}

	channel_rec = kmalloc(sizeof(struct iccom_message_storage_channel)
			      , GFP_KERNEL);
	if (!channel_rec) {
		iccom_warning("No memory to create new channel.");
		return NULL;
	}

	// initialization
	list_add_tail(&(channel_rec->channel_anchor)
		      , &(storage->channels_list));
	channel_rec->channel = channel;
	INIT_LIST_HEAD(&channel_rec->messages);
	channel_rec->current_last_message_id
			= ICCOM_PACKET_INVALID_MESSAGE_ID;
	channel_rec->consumer_callback_data = NULL;
	channel_rec->message_ready_callback = NULL;

	return channel_rec;
}

// RETURNS:
//      true: channel has no consumer/consumer dedicated data
//          (and thus can be freed without any data loss)
//      false: channel contains some consumer data and can not
//          be freed witout data loss
static inline bool iccom_msg_storage_channel_has_no_data(
	struct iccom_message_storage_channel *channel_rec)
{
	return list_empty(&(channel_rec->messages))
		    && !channel_rec->consumer_callback_data
		    && !channel_rec->message_ready_callback;
}

// Helper. Removes given channel from the storage and discards all
// its messages, other pointed data and channel structure itself.
//
// LOCKING: storage should be locked before calling this function
static void __iccom_msg_storage_free_channel(
	struct iccom_message_storage_channel *channel_rec)
{
	if (!channel_rec) {
		return;
	}

	while (!list_empty(&(channel_rec->messages))) {
		struct iccom_message *msg_rm
			= container_of(channel_rec->messages.next
				       , struct iccom_message
				       , list_anchor);
		__iccom_message_free(msg_rm);
	}

	list_del(&(channel_rec->channel_anchor));
	kfree(channel_rec);

	return;
}

// Notifies the channel consumer about all ready messages
// in the channel (in FIFO sequence). Notified messages are
// discarded from the channel if consumer callback
// returns true.
//
// Should be executed in async mode to avoid blocking of underlying
// layer (full duplex transport) by upper layers (data consumer).
//
// If channel has no callback installed, then its messages are
// ignored and stay in the storage. TODO: if there are more than
// fixed number of messages in the channel pending to consumer layer
// then incoming messages are to be dropped and channel overflow
// signalized.
//
// @channel_rec {valid ptr} pointer to the channel to work with
//
// LOCKING:
// 	TODO: CLARIFY: storage should be locked before calling this
// 		function
//
// RETURNS:
//      >=0: number of messages processed: (notified and then discarded)
//      <0: negqated error code
static int __iccom_msg_storage_pass_channel_to_consumer(
		struct iccom_message_storage *storage
		, struct iccom_message_storage_channel *channel_rec)
{
	if (IS_ERR_OR_NULL(channel_rec)) {
		return -EINVAL;
	}

	iccom_msg_ready_callback_ptr_t msg_ready_callback;
	void *callback_consumer_data;

	mutex_lock(&storage->lock);
	if (!IS_ERR_OR_NULL(channel_rec->message_ready_callback)) {
		msg_ready_callback = channel_rec->message_ready_callback;
		callback_consumer_data = channel_rec->consumer_callback_data;
	} else if (!IS_ERR_OR_NULL(storage->message_ready_global_callback)) {
		msg_ready_callback = storage->message_ready_global_callback;
		callback_consumer_data = storage->global_consumer_data;
	} else {
		mutex_unlock(&storage->lock);
		return 0;
	}

	int count = 0;
	struct iccom_message *msg;

	// NOTE: the only guy to remove the message from the
	// 	storage is us, so if we unlock the mutex while our
	// 	consumer deals with the message, we only allow
	// 	to add new messages into the storage, while removing
	// 	them is our responsibility, so we shall not face with
	// 	the issue that we step onto message which will suddenly
	// 	be removed.
	list_for_each_entry(msg, &channel_rec->messages
			    , list_anchor) {
		if (!__iccom_message_is_ready(msg)) {
			continue;
		}
		mutex_unlock(&storage->lock);

		count++;
		bool ownership_to_consumer = msg_ready_callback(
						 channel_rec->channel
						 , msg->data, msg->length
						 , callback_consumer_data);
		if (ownership_to_consumer) {
			msg->data = NULL;
			msg->length = 0;
		}

		mutex_lock(&storage->lock);
		// removing notified message from the storage
		struct iccom_message *prev;
		prev = container_of(msg->list_anchor.prev
				    , struct iccom_message, list_anchor);
		// @@@@@@@@@@@@@@@@@@@ TODO: verify locks
		__iccom_message_free(msg);
		msg = prev;
	}
	mutex_unlock(&storage->lock);

	return count;
}

// Helper. Allocates the next message id for the channel.
// Returns the value of the next message id for the channel
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to work with
//
// LOCKING: storage should be locked before calling this function
//
// RETURNS:
//      >=ICCOM_PACKET_INITIAL_MESSAGE_ID the value of the next
//          message id for the channel
//      if channel was not found also returns
//          ICCOM_PACKET_INITIAL_MESSAGE_ID
static unsigned int __iccom_msg_storage_allocate_next_msg_id(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return -ENODEV);
	ICCOM_CHECK_CHANNEL("", return -EBADSLT);
#endif

	struct iccom_message_storage_channel *channel_rec;
	channel_rec = __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		return ICCOM_PACKET_INITIAL_MESSAGE_ID;
	}
	unsigned int next_id;
	if (list_empty(&channel_rec->messages)) {
		next_id = ICCOM_PACKET_INITIAL_MESSAGE_ID;
		channel_rec->current_last_message_id = next_id;
		return next_id;
	}

	next_id = channel_rec->current_last_message_id + 1;
	if (next_id == 0) {
		next_id = ICCOM_PACKET_INITIAL_MESSAGE_ID;
	}
	channel_rec->current_last_message_id = next_id;

	return next_id;
}

// Tries to find a message given by its channel and message id
// in storage.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
// @msg_id {number} the id of the message to retrieve
//
// LOCKING: storage should be locked before this call
//
// RETURNS:
//      !NULL pointer to message - if the message is found
//      NULL pointer - if no message was found
static inline struct iccom_message *__iccom_msg_storage_get_message(
		struct iccom_message_storage *storage
		, unsigned int channel
		, unsigned int msg_id)
{
	struct iccom_message_storage_channel *channel_rec;
	channel_rec = __iccom_msg_storage_find_channel(storage, channel);
	return __iccom_msg_storage_find_message_in_channel(
					channel_rec, msg_id);
}

// Helper. Rolls back all uncommitted message data in channel.
//
// LOCKING: needs storage to be locked before the call, by caller
static void __iccom_msg_storage_channel_rollback(
	struct iccom_message_storage_channel *channel_rec)
{
	struct iccom_message *msg;

	list_for_each_entry(msg, &channel_rec->messages, list_anchor) {
		if (msg->uncommitted_length == 0) {
			continue;
		}
		// no reallocation needed, as long as every time new
		// data added or message freed the old area is freed
		// and its length is managed by slab allocator
		msg->finalized = false;
		msg->length -= msg->uncommitted_length;
		msg->uncommitted_length = 0;
	}
}

// Helper. Commits all uncommitted changes in the channel.
//
// LOCKING: needs storage to be locked before the call, by caller
static void __iccom_msg_storage_channel_commit(
	struct iccom_message_storage_channel *channel_rec)
{
	struct iccom_message *msg;

	list_for_each_entry(msg, &channel_rec->messages, list_anchor) {
		if (msg->uncommitted_length == 0) {
			continue;
		}
		msg->uncommitted_length = 0;
	}
}

#ifdef ICCOM_DEBUG
// @max_printout_count {>=-1}, maximum number of msgs to print total,
//      -1 means "unlimited", 0 means "do not print"
// LOCKING: needs storage to be locked before the call, by caller
static int __iccom_msg_storage_printout_channel(
		struct iccom_message_storage_channel *channel_rec
		, int max_printout_count)
{
	if (!max_printout_count) {
		return 0;
	}
	int printed = 0;
	struct iccom_message *msg;
	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL
		       , "== CH: %d ==", channel_rec->channel);
	list_for_each_entry(msg, &channel_rec->messages, list_anchor) {
		if (max_printout_count > 0 && printed >= max_printout_count) {
			iccom_warning_raw("CHANNEL PRINTOUT CUTOFF");
			break;
		}
		iccom_dbg_printout_message(msg);
		printed++;
	}
	return printed;
}

// @max_printout_count {>=-1}, maximum number of msgs to print total,
//      -1 means "unlimited", 0 means "do not print"
// @channel {>=-1} channel to print, -1 means all
//
// LOCKING: needs storage to be locked before the call, by caller
static int __iccom_msg_storage_printout(
		struct iccom_message_storage *storage
		, int max_printout_count
		, int channel)
{
	if (!max_printout_count) {
		return 0;
	}
	int printed = 0;
	struct iccom_message_storage_channel *channel_rec;
	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL
		       , "== Messages Storage ==");
	if (max_printout_count < 0) {
		list_for_each_entry(channel_rec, &storage->channels_list
				    , channel_anchor) {
			if (channel >= 0 && channel_rec->channel != channel) {
				continue;
			}
			printed += __iccom_msg_storage_printout_channel(
							 channel_rec, 0);
		}
		goto done;
	}

	list_for_each_entry(channel_rec, &storage->channels_list
			    , channel_anchor) {
		if (printed >= max_printout_count) {
			iccom_warning_raw("MESSAGES STORAGE PRINTOUT CUTOFF");
			break;
		}
		if (channel >= 0 && channel_rec->channel != channel) {
			continue;
		}
		printed += __iccom_msg_storage_printout_channel(
				channel_rec, max_printout_count - printed);
	}
done:
	iccom_info_raw(ICCOM_LOG_INFO_DBG_LEVEL
		       , "== Messages Storage END ==");
	return printed;
}
#endif
/* ------------------ MESSAGES STORE API --------------------------------*/

// Tries to find a message given by its channel and message id
// in storage.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
// @msg_id {number} the id of the message to retrieve
//
// CONCURRENCE: thread safe
// OWNERSHIP: the message ownership remains belong to storage
//
// RETURNS:
//      !NULL pointer to message - if the message is found
//      NULL pointer - if no message was found
__maybe_unused
struct iccom_message *iccom_msg_storage_get_message(
		struct iccom_message_storage *storage
		, unsigned int channel
		, unsigned int msg_id)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return NULL);
	ICCOM_CHECK_CHANNEL("", return NULL);
#endif
	mutex_lock(&storage->lock);
	struct iccom_message *res = __iccom_msg_storage_get_message(
					    storage, channel, msg_id);
	mutex_unlock(&storage->lock);
	return res;
}

// Returns the yongest message in the channel (if one),
// if there is no messages - returns NULL. The youngest
// message may, surely, be unfinished.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
//
// CONCURRENCE: thread safe
// OWNERSHIP: the message ownership remains belong to storage
//
// RETURNS:
//      !NULL pointer to message - if the message is found
//      NULL pointer - if no message was found (or even if
//          no relevant channel found)
__maybe_unused
struct iccom_message *iccom_msg_storage_get_last_message(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return NULL);
	ICCOM_CHECK_CHANNEL("", return NULL);
#endif
	struct iccom_message *msg = NULL;
	struct iccom_message_storage_channel *channel_rec;

	mutex_lock(&storage->lock);
	channel_rec = __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		goto finalize;
	}
	if (list_empty(&channel_rec->messages)) {
		goto finalize;
	}

	msg = container_of(channel_rec->messages.prev
			   , struct iccom_message, list_anchor);

finalize:
	mutex_unlock(&storage->lock);
	return msg;
}

// Returns the yongest message in the channel if it is not finalized;
// if there is no such message - returns NULL.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
//
// CONCURRENCE: thread safe
// OWNERSHIP: the message ownership remains belong to storage
//
// RETURNS:
//      !NULL pointer to message - if the message is found
//      NULL pointer - if no message was found (or even if
//          no relevant channel found)
__maybe_unused
struct iccom_message *iccom_msg_storage_get_last_unfinalized_message(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return NULL);
	ICCOM_CHECK_CHANNEL("", return NULL);
#endif
	struct iccom_message *msg = NULL;
	struct iccom_message_storage_channel *channel_rec;

	mutex_lock(&storage->lock);
	channel_rec = __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		goto finalize;
	}
	if (list_empty(&channel_rec->messages)) {
		goto finalize;
	}

	msg = container_of(channel_rec->messages.prev
			   , struct iccom_message, list_anchor);
	if (msg->finalized) {
		msg = NULL;
	}

finalize:
	mutex_unlock(&storage->lock);
	return msg;
}


// Returns the oldest message in the channel (if one),
// if there is no messages - returns NULL. The oldest
// message may still be unfinished.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
//
// CONCURRENCE: thread safe
// OWNERSHIP: the message ownership remains belong to storage
//
// NOTE: the ownership of the messages is still belongs to
//      the storage.
//
// RETURNS:
//      !NULL pointer to message - if the message is found
//      NULL pointer - if no message was found (or even if
//          no relevant channel found)
__maybe_unused
struct iccom_message *iccom_msg_storage_get_first_message(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return NULL);
	ICCOM_CHECK_CHANNEL("", return NULL);
#endif
	struct iccom_message *msg = NULL;
	struct iccom_message_storage_channel *channel_rec;

	mutex_lock(&storage->lock);
	channel_rec = __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		goto finalize;
	}
	if (list_empty(&channel_rec->messages)) {
		goto finalize;
	}

	msg = container_of(channel_rec->messages.next
			   , struct iccom_message, list_anchor);
finalize:
	mutex_unlock(&storage->lock);
	return msg;
}

// Returns the oldest finalized message in the channel (if one),
// if there is no such messages or channel - returns NULL.
// The finalized messages with uncommitted data are ignored.
// Message remains in the storage.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
//
// CONCURRENCE: thread safe
// OWNERSHIP: the message ownership remains belong to storage
//
// RETURNS:
//      !NULL pointer to message - if the message is found
//      NULL pointer - if no message was found (or even if
//          no relevant channel found, or channel number
//          is invalid)
__maybe_unused
struct iccom_message *iccom_msg_storage_get_first_ready_message(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return NULL);
	ICCOM_CHECK_CHANNEL("", return NULL);
#endif
	struct iccom_message *msg = NULL;
	struct iccom_message_storage_channel *channel_rec;

	mutex_lock(&storage->lock);
	channel_rec = __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		goto finalize;
	}
	if (list_empty(&channel_rec->messages)) {
		goto finalize;
	}

	list_for_each_entry(msg, &channel_rec->messages
			    , list_anchor) {
		if (__iccom_message_is_ready(msg)) {
			goto finalize;
		}
	}
	msg = NULL;
finalize:
	mutex_unlock(&storage->lock);
	return msg;
}

// Pops the oldest finalized message in the channel (if one),
// if there is no such messages or channel - returns NULL.
// The finalized messages with uncommitted data are ignored.
// Message is removed from the storage
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
//
// CONCURRENCE: thread safe
// OWNERSHIP: the message ownership is transferred to the caller
//
// RETURNS:
//      !NULL pointer to message - if the message is found
//      NULL pointer - if no message was found (or even if
//          no relevant channel found, or channel number
//          is invalid)
__maybe_unused
struct iccom_message *iccom_msg_storage_pop_first_ready_message(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return NULL);
	ICCOM_CHECK_CHANNEL("", return NULL);
#endif
	struct iccom_message *msg = NULL;
	struct iccom_message_storage_channel *channel_rec;

	mutex_lock(&storage->lock);
	channel_rec = __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		goto finalize;
	}
	if (list_empty(&channel_rec->messages)) {
		goto finalize;
	}

	list_for_each_entry(msg, &channel_rec->messages
			    , list_anchor) {
		if (__iccom_message_is_ready(msg)) {
			list_del(&(msg->list_anchor));
			goto finalize;
		}
	}
	msg = NULL;
finalize:
	mutex_unlock(&storage->lock);
	return msg;
}

// Removes the message from the storage and returns pointer to
// the popped message. Messages ownership is transferred to the
// caller.
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
//
// CONCURRENCE: thread safe
// OWNERSHIP: the message ownership is transferred to the storage
//
// RETURNS:
//      !NULL pointer to the popped message, if found
//      NULL, if no message was found or call parameters
//          are invalid
__maybe_unused
struct iccom_message *iccom_msg_storage_pop_message(
		struct iccom_message_storage *storage
		, unsigned int channel
		, unsigned int msg_id)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return NULL);
	ICCOM_CHECK_CHANNEL("", return NULL);
#endif
	struct iccom_message *msg = NULL;

	mutex_lock(&storage->lock);

	struct iccom_message_storage_channel * channel_rec
		= __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		goto finalize;
	}

	msg = __iccom_msg_storage_find_message_in_channel(
					channel_rec, msg_id);
	if (!msg) {
		goto finalize;
	}

	list_del(&(msg->list_anchor));

	// NOTE: we will save the channel record to avoid excessive
	//      memory de-re-allocations, as long as channel with
	//      high probability will persist and be reused.
	//
	// TODO: add last use timestamp to the channel record to
	//      remove it after long enough idle period.

finalize:
	mutex_unlock(&storage->lock);
	return msg;
}

// Adds new message to the message storage. The message ownership
// is transferred to the storage. While not provided externally
// by protocol, automatically assignes message ID.
//
// @storage {valid ptr} the pointer to the storage to use
// @msg {valid ptr to heap region} message to add to the storage,
//      required to have valid channel id set.
//      Message struct MUST be dynamically allocated.
//
// CONCURRENCE: thread safe
// OWNERSHIP: the message ownership is transferred to the storage
//
// RETURNS: 0 - if successfully added message to the storage
//          <0 - negated error code, if failed
//              -ENOMEM: no memory to allocate new channel
//              -EALREADY: message already exists
//
__maybe_unused
int iccom_msg_storage_push_message(
		struct iccom_message_storage __kernel *storage
		, struct iccom_message __kernel *msg)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return -ENODEV);
	ICCOM_CHECK_PTR(msg, return -EINVAL);
	ICCOM_CHECK_CHANNEL_EXT(msg->channel, "", return -EBADSLT);
#endif
	mutex_lock(&storage->lock);

	int res = 0;
	struct iccom_message_storage_channel * channel_rec
			= __iccom_msg_storage_add_channel(storage
							  , msg->channel);
	if (!channel_rec) {
		iccom_err("%s: no memory for channel", __func__);
		res = -ENOMEM;
		goto finalize;
	}

	if (__iccom_msg_storage_find_message_in_channel(
			channel_rec, msg->id)) {
		iccom_err("Could not put a message with id %x"
			  " to %x channel: message already exists"
			  , msg->id, msg->channel);
		res = -EALREADY;
		goto finalize;
	}

	list_add_tail(&(msg->list_anchor), &(channel_rec->messages));

	// TODO: as protocol contains the message id, then it should
	//      be set externally (not by storage)
	//
	// while message id is not used in protocol we just generate
	// new message ids by our-selves
	// TODO: no need to search for the channel for the second time
	msg->id = __iccom_msg_storage_allocate_next_msg_id(
			storage, msg->channel);

finalize:
	mutex_unlock(&storage->lock);
	return res;
}

// Removes the message from its storage.
//
// NOTE: TODO: thread safe
__maybe_unused
void iccom_msg_storage_remove_message(struct iccom_message *msg)
{
	list_del(&(msg->list_anchor));
}


// Removes all unused channel records from the storage.
//
// NOTE: TODO: thread safe
//
// NOTE: Later it may perform additional cleanup inside the
// channel.
__maybe_unused
void iccom_msg_storage_collect_garbage(
		struct iccom_message_storage *storage)
{
	if (list_empty(&storage->channels_list)) {
		return;
	}

	struct iccom_message_storage_channel *channel_rec
		= __iccom_msg_storage_anchor2channel(
			storage->channels_list.next);

	while (true) {
		struct list_head *next = channel_rec->channel_anchor.next;
		if (iccom_msg_storage_channel_has_no_data(channel_rec)) {
			list_del(&channel_rec->channel_anchor);
			kfree(channel_rec);
		}
		if (next == &storage->channels_list) {
			break;
		}
		channel_rec = __iccom_msg_storage_anchor2channel(next);
	}
}

// Removes the channel from the storage (with all attached
// messages).
//
// CONCURRENCE: thread safe
//
// @storage {valid ptr} the pointer to the messages storage
// @channel {number} the channel number to search the message in
__maybe_unused
void iccom_msg_storage_remove_channel(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return);
	ICCOM_CHECK_CHANNEL_EXT(channel, "", return);
#endif
	mutex_lock(&storage->lock);
	struct iccom_message_storage_channel *channel_rec
		= __iccom_msg_storage_find_channel(storage, channel);
	__iccom_msg_storage_free_channel(channel_rec);
	mutex_unlock(&storage->lock);
}

// Cleans whole storage (all channels and contained messages are
// removed and freed). Including all callback related information.
//
// NOTE: thread safe
__maybe_unused
void iccom_msg_storage_clear(struct iccom_message_storage *storage)
{
	mutex_lock(&storage->lock);
	while (!list_empty(&storage->channels_list)) {
		struct list_head *first = storage->channels_list.next;
		__iccom_msg_storage_free_channel(
			__iccom_msg_storage_anchor2channel(first));
	}
	storage->uncommitted_finalized_count = 0;
	mutex_unlock(&storage->lock);
	storage->message_ready_global_callback = NULL;
	storage->global_consumer_data = NULL;
}

// Cleans and frees whole storage (the struct itself it not freed).
// NOTE: should be called only on closing of the ICCom driver,
//      when all calls which might affect storage are blocked.
__maybe_unused
void iccom_msg_storage_free(struct iccom_message_storage *storage)
{
	iccom_msg_storage_clear(storage);
	mutex_destroy(&storage->lock);
}

// Appends the data to the registered message, and updates
// the finalizes flag and uncommitted_length fields of
// iccom_message.
//
// @storage {valid storage pointer} storage to work with
// @channel {number} channel number to work with
// @msg_id {number} message id to attach the new data to
// @new_data {valid new data pointer} pointer to data to append
// @new_data_length {correct new data length} in bytes
// @final indicates if the message should be finalized (works
//      only once for the message)
//
// CONCURRENCE: thread safe, but consumer must ensure that
//      no update work with non-finalized messages
//      will be performed while data appending (this is for now a case)
//      (or at least on the message under update)
//
// RETURNS:
//      0: on success
//      <0: Negated error code if fails
__maybe_unused
int iccom_msg_storage_append_data_to_message(
	    struct iccom_message_storage *storage
	    , unsigned int channel, unsigned int msg_id
	    , void *new_data, size_t new_data_length
	    , bool final)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return -ENODEV);
	ICCOM_CHECK_CHANNEL("", return -EBADSLT);
	if (IS_ERR_OR_NULL(new_data)) {
		iccom_err("%s: new message data pointer broken", __func__);
		return -EINVAL;
	}
	if (new_data_length == 0) {
		iccom_err("%s: new message data length is 0", __func__);
		return -EINVAL;
	}
#endif
	mutex_lock(&storage->lock);
	struct iccom_message *msg = __iccom_msg_storage_get_message(
					    storage, channel, msg_id);
	if (IS_ERR_OR_NULL(msg)) {
		iccom_err("No such message to extend: channel %x"
			  ", id %x", channel, msg_id);
		mutex_unlock(&storage->lock);
		return -EBADF;
	}

	if (msg->finalized) {
		iccom_err("Can not add data to finalized message"
			  "(channel %x, msg id %x)", channel, msg_id);
		mutex_unlock(&storage->lock);
		return -EACCES;
	}

	mutex_unlock(&storage->lock);

	// We unlock here cause we have a contract with storage consumer:
	// to not to modify/delete unfinalized messages. Our aim is to
	// keep all heavy operations (like memcpy) out of lock.

	// TODO: avoid reallocation (allocate maximum available message
	// size only once).
	void *new_store = kmalloc(msg->length + new_data_length, GFP_KERNEL);
	if (!new_store) {
		iccom_err("Could not allocate memory for new message data.");
		return -ENOMEM;
	}

	if (!IS_ERR_OR_NULL(msg->data) && msg->length > 0) {
		memcpy(new_store, msg->data, msg->length);
	}
	memcpy(new_store + msg->length, new_data, new_data_length);

	// caution: the order of lines matters here: we update the pointer
	// first to keep the data selfconsistent, cause new data block
	// contains the old one, thus the data still will be selfconsistent
	char *old_data = msg->data;
	msg->data = new_store;
	mutex_lock(&storage->lock);
	msg->length += new_data_length;
	msg->uncommitted_length = new_data_length;
	mutex_unlock(&storage->lock);
	kfree(old_data);

	if (final) {
		msg->finalized = true;
		__sync_add_and_fetch(&storage->uncommitted_finalized_count, 1);
	}

	return 0;
}

// Sets the channel callback. If previous exists it is overwritten.
//
// @channel {valid channel value | ICCOM_ANY_CHANNEL_VALUE}
//      the channel to install the callback; if equals to
//      ICCOM_ANY_CHANNEL_VALUE then callback is installed
//      as global callback for the whole storage.
//
// CONCURRENCE: thread safe
//
// RETURNS:
//      0: on success
//      <0: negated error code
__maybe_unused
static int iccom_msg_storage_set_channel_callback(
		struct iccom_message_storage *storage
		, unsigned int channel
		, iccom_msg_ready_callback_ptr_t message_ready_callback
		, void *consumer_data)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return -ENODEV);
	ICCOM_CHECK_CHANNEL("", return -EBADSLT);
	if (IS_ERR(message_ready_callback)) {
		iccom_err("broken message ready callback ptr");
		return -EINVAL;
	}
#endif
	if (channel == ICCOM_ANY_CHANNEL_VALUE) {
		mutex_lock(&storage->lock);
		storage->message_ready_global_callback
				= message_ready_callback;
		storage->global_consumer_data
				= consumer_data;
		mutex_unlock(&storage->lock);
		return 0;
	}

	mutex_lock(&storage->lock);

	int res = 0;
	struct iccom_message_storage_channel *channel_rec
		= __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		if (message_ready_callback == NULL) {
			goto finalize;
		}
		channel_rec = __iccom_msg_storage_add_channel(storage
							      , channel);
		if (!channel_rec) {
			iccom_err("%s: no memory for channel", __func__);
			res = -ENOMEM;
			goto finalize;
		}
	}
	channel_rec->consumer_callback_data = consumer_data;
	channel_rec->message_ready_callback = message_ready_callback;

finalize:
	mutex_unlock(&storage->lock);
	return res;
}

// Resets the channel callback.
// RETURNS:
//      0: on success
//      <0: negated error code
__maybe_unused
static inline int iccom_msg_storage_reset_channel_callback(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
	return iccom_msg_storage_set_channel_callback(storage
			, channel, NULL, NULL);
}

// Gets the channel callback.
//
// CONCURRENCE: thread safe
//
// RETURNS:
//      ERR PTR: on failure
//      NULL: if channel doesn't exist || callback is not set
//      callback pointer: if channel exists and callback is set
__maybe_unused
static  iccom_msg_ready_callback_ptr_t
iccom_msg_storage_get_channel_callback(
		struct iccom_message_storage *storage
		, unsigned int channel)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return ERR_PTR(-ENODEV));
	ICCOM_CHECK_CHANNEL("", return ERR_PTR(-EBADSLT));
#endif

	if (channel == ICCOM_ANY_CHANNEL_VALUE) {
		return storage->message_ready_global_callback;
	}

	mutex_lock(&storage->lock);

	iccom_msg_ready_callback_ptr_t res = NULL;
	struct iccom_message_storage_channel *channel_rec
		= __iccom_msg_storage_find_channel(storage, channel);
	if (!channel_rec) {
		goto finalize;
	}
	res = channel_rec->message_ready_callback;
finalize:
	mutex_unlock(&storage->lock);
	return res;
}

// Invokes callbacks of all channels with finished messages.
// If callback returns true, then message is discarded from
// the storage.
//
// RETURNS:
//      >=0: how many messages were notified and discarded
//      <0: negated error code
__maybe_unused
static inline int iccom_msg_storage_pass_ready_data_to_consumer(
		struct iccom_message_storage *storage)
{
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return -ENODEV);

	struct iccom_message_storage_channel *channel_rec;
	int count = 0;
	list_for_each_entry(channel_rec, &storage->channels_list
			    , channel_anchor) {
		int res = __iccom_msg_storage_pass_channel_to_consumer(
						  storage, channel_rec);
		if (res < 0) {
			iccom_err("consumer notification failed err: %d", res);
			return res;
		}
		count += res;
	}

	return count;
}

// Rolls back the uncommitted changes in the storage.
// Needs to cleanup the storage from data which was taken from
// broken package to avoid the data duplication when other
// side sends us the same package for the second time.
//
// CONCURRENCE: thread safe
//
// NOTE: as long as broken package is pretty rare situation,
//      the function is not intended to be very time efficient
//      it just scans whole storage and rolls back all uncommitted
//      data
__maybe_unused
static void iccom_msg_storage_rollback(
	    struct iccom_message_storage *storage)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return);
#endif
	struct iccom_message_storage_channel *channel_rec;
	mutex_lock(&storage->lock);
	list_for_each_entry(channel_rec, &storage->channels_list
			    , channel_anchor) {
		__iccom_msg_storage_channel_rollback(channel_rec);
	}
	mutex_unlock(&storage->lock);
}

// Commits all uncommitted changes in the storage.
//
// CONCURRENCE: thread safe
__maybe_unused
static void iccom_msg_storage_commit(
	    struct iccom_message_storage *storage)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return);
#endif
	struct iccom_message_storage_channel *channel_rec;
	mutex_lock(&storage->lock);
	list_for_each_entry(channel_rec, &storage->channels_list
			    , channel_anchor) {
		__iccom_msg_storage_channel_commit(channel_rec);
	}
	storage->uncommitted_finalized_count = 0;
	mutex_unlock(&storage->lock);
}

// Initializes the message storage
// RETURNS:
//      0: all fine
//      <0: negative error code
__maybe_unused
static int iccom_msg_storage_init(
	    struct iccom_message_storage *storage)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return -EINVAL);
#endif
	INIT_LIST_HEAD(&storage->channels_list);
	mutex_init(&storage->lock);
	storage->uncommitted_finalized_count = 0;
	storage->message_ready_global_callback = NULL;
	storage->global_consumer_data = NULL;
	return 0;
}

// CONCURRENCE: thread safe
// RETURNS:
//      >=0: number of finalized since last commit messages
//      <0: negated error code
__maybe_unused
static inline int iccom_msg_storage_uncommitted_finalized_count(
	    struct iccom_message_storage *storage)
{
#ifdef ICCOM_DEBUG
	ICCOM_MSG_STORAGE_CHECK_STORAGE("", return -ENODEV);
#endif
	return storage->uncommitted_finalized_count;
}

/* -------------------------- UTILITIES ---------------------------------*/

// Helper.
// Initializes the report error array.
static void __iccom_error_report_init(struct iccom_dev *iccom)
{
	memset(iccom->p->errors, 0, sizeof(iccom->p->errors));

#define ICCOM_ERR_REC(idx, ERR_NAME, threshold_err_per_sec)		\
	iccom->p->errors[idx].err_num = ICCOM_ERROR_##ERR_NAME;		\
	iccom->p->errors[idx].err_msg					\
		= (const char*)&ICCOM_ERROR_S_##ERR_NAME;		\
	iccom->p->errors[idx].err_per_sec_threshold			\
		= threshold_err_per_sec;

	ICCOM_ERR_REC(0, NOMEM, 0);
	ICCOM_ERR_REC(1, TRANSPORT, 5);

#undef ICCOM_ERR_REC
}

// Helper.
// Returns the error record pointer given by error number
//
// @iccom {valid ptr to iccom device}
// @err_no {the valid error number to report}
static inline struct iccom_error_rec *__iccom_get_error_rec(
		struct iccom_dev *iccom, unsigned char err_no)
{
	ICCOM_CHECK_DEVICE_PRIVATE("no device", return NULL);

	for (int i = 0; i < ARRAY_SIZE(iccom->p->errors); ++i) {
		if (iccom->p->errors[i].err_num == err_no) {
			return &(iccom->p->errors[i]);
		}
	}
	return NULL;
}

static inline u64 time_divns(const ktime_t kt, s64 div)
{
	u64 dclc;
	int sft = 0;

	dclc = ktime_to_ns(kt);
	/* Make sure the divisor is less than 2^32: */
	while (div >> 32) {
		sft++;
		div >>= 1;
	}
	dclc >>= sft;
	do_div(dclc, (unsigned long) div);

	return dclc;
}

// Helper.
// Reports error to the kernel log and also tracks the history of errors
// and protects kernel log from error messages flood in case of external
// errors triggering.
//
// @iccom {valid ptr to iccom device}
// @err_no {the valid error number to report}
// @sub_error_no subsystem error number (might be used to pass
//      subsystem error code)
// @func_name {NULL || valid string pointer} function name where
//      the error was raised
//
// RETURNS:
//      true: if it is OK to be verbose now
//      false: else (silence required)
static bool __iccom_error_report(struct iccom_dev *iccom
				  , unsigned char err_no
				  , int sub_error_no
				  , const char *func_name)
{
	ICCOM_CHECK_DEVICE_PRIVATE("no device", return true);

	struct iccom_error_rec *e_ptr = __iccom_get_error_rec(iccom
							      , err_no);
	if (e_ptr == NULL) {
		iccom_err("unknown error type given: %u" , err_no);
		return true;
	}

	// NOTE: wraps every ~24 hours
	const uint32_t now_msec = (uint32_t)(time_divns(ktime_get(), 1000000));
	e_ptr->total_count++;

	const unsigned int since_last_report_msec
			= (now_msec >= e_ptr->last_report_time_msec)
			  ? (now_msec - e_ptr->last_report_time_msec)
			  : (e_ptr->last_report_time_msec - now_msec);
	const unsigned int since_last_occurrence_msec
			= (now_msec >= e_ptr->last_occurrence_time_msec)
			  ? (now_msec - e_ptr->last_occurrence_time_msec)
			  : (e_ptr->last_occurrence_time_msec - now_msec);
	e_ptr->last_occurrence_time_msec = now_msec;

	// approximately calculating the decay rate at this time point
	// surely it will not be exactly the exp decay, but will resemble
	// the general behaviour
	const unsigned int decay_percent
		= max(min((unsigned int)((50 * since_last_occurrence_msec)
					 / ICCOM_ERR_RATE_DECAY_RATE_MSEC_PER_HALF)
			  , (unsigned int)100)
		      , (unsigned int)ICCOM_ERR_RATE_DECAY_RATE_MIN);
	const unsigned int threshold = e_ptr->err_per_sec_threshold;
	const unsigned int prev_rate
		= 1000 / max((unsigned int)(e_ptr->exp_avg_interval_msec), 1U);

	e_ptr->exp_avg_interval_msec
		= max((unsigned int)(((100 - decay_percent)
				        * e_ptr->exp_avg_interval_msec
		      		      + decay_percent
				        * since_last_occurrence_msec) / 100)
		      , 1U);

	const unsigned int rate = 1000 / e_ptr->exp_avg_interval_msec;

#ifdef ICCOM_DEBUG
	iccom_err_raw("====== error %d ======", err_no);
	iccom_err_raw("diff interval: %u", since_last_occurrence_msec);
	iccom_err_raw("decay percent: %u", decay_percent);
	iccom_err_raw("new avg interval: %lu", e_ptr->exp_avg_interval_msec);
	iccom_err_raw("rate_prev = %u", prev_rate);
	iccom_err_raw("rate = %u", rate);
#endif

	if (since_last_report_msec < ICCOM_MIN_ERR_REPORT_INTERVAL_MSEC
			&& !(prev_rate < threshold && rate >= threshold)) {
		e_ptr->unreported_count++;
		e_ptr->last_reported = false;
		return false;
	}

	e_ptr->last_report_time_msec = now_msec;
	e_ptr->last_reported = true;

	static const char *const level_err = "error";
	static const char *const level_warn = "warning";
	const char *const report_class_str = (rate >= threshold)
					     ? level_err : level_warn;

	if (func_name) {
		iccom_err_raw("ICCom %s %u (avg. rate per sec: %d): "
			      "%s (sub %s: %d), raised by %s"
			      , report_class_str, err_no
			      , rate, e_ptr->err_msg, report_class_str
			      , sub_error_no, func_name);
	} else {
		iccom_err_raw("ICCom %s %u (avg. rate per sec: %d): "
			      "%s (sub %s: %d)"
			      , report_class_str, err_no
			      , rate, e_ptr->err_msg, report_class_str
			      , sub_error_no);
	}

	if (e_ptr->unreported_count > 0) {
		iccom_err_raw("meanwhile, %s %d happened %d times"
			      " since last reporting %u msecs ago. Total "
			      "count is %u.", report_class_str, err_no
			      , e_ptr->unreported_count
			      , since_last_report_msec, e_ptr->total_count);
		e_ptr->unreported_count = 0;
	}

	return true;
}

// Helper.
// Inits the workqueue which is to be used by ICCom
// in its current configuration. If we use system-provided
// workqueue - does nothing.
//
// RETURNS:
//      >= 0     - on success
//      < 0     - negative error code
//
// ERRORS:
//      EAGAIN if workqueue init fails
static inline int __iccom_init_workqueue(
		const struct iccom_dev __kernel *const iccom)
{
#if ICCOM_WORKQUEUE_MODE_MATCH(SYSTEM)
	iccom_info(ICCOM_LOG_INFO_KEY_LEVEL, "using system wq");
	(void)iccom;
	return 0;
#elif ICCOM_WORKQUEUE_MODE_MATCH(SYSTEM_HIGHPRI)
	iccom_info(ICCOM_LOG_INFO_KEY_LEVEL, "using system_highpri wq");
	(void)iccom;
	return 0;
#elif ICCOM_WORKQUEUE_MODE_MATCH(PRIVATE)
	iccom_info(ICCOM_LOG_INFO_KEY_LEVEL, "using private wq");
	iccom->p->work_queue = alloc_workqueue("iccom", WQ_HIGHPRI, 0);

	if (iccom->p->work_queue) {
		return 0;
	}

	iccom_err("%s: the private work queue init failed."
				, __func__);
	return -EAGAIN;
#endif
}

// Helper.
// Closes the workqueue which was used by SymSPI
// in its current configuration. If we use system-provided
// workqueu - does nothing.
static inline void __iccom_close_workqueue(
		const struct iccom_dev *const iccom)
{
#if ICCOM_WORKQUEUE_MODE_MATCH(PRIVATE)
	destroy_workqueue(iccom->p->work_queue);
	iccom->p->work_queue = NULL;
#else
	(void)iccom;
#endif
}

// Helper.
// Wrapper over schedule_work(...) for queue selected by configuration.
// Schedules SymSPI work to the target queue.
static inline void __iccom_schedule_work(
		const struct iccom_dev *const iccom
		, struct work_struct *work)
{
#if ICCOM_WORKQUEUE_MODE_MATCH(SYSTEM)
	(void)iccom;
	schedule_work(work);
#elif ICCOM_WORKQUEUE_MODE_MATCH(SYSTEM_HIGHPRI)
	(void)iccom;
	queue_work(system_highpri_wq, work);
#elif ICCOM_WORKQUEUE_MODE_MATCH(PRIVATE)
	queue_work(iccom->p->work_queue, work);
#else
#error no known SymSPI work queue mode defined
#endif
}

// Helper.
// Wrapper over cancel_work_sync(...) in case we will
// need some custom queue operations on cancelling.
static inline void __iccom_cancel_work_sync(
		const struct iccom_dev *const iccom
		, struct work_struct *work)
{
	cancel_work_sync(work);
}

// Helper. Provides next outgoing package id.
static int __iccom_get_next_package_id(struct iccom_dev *iccom)
{
	int pkg_id = iccom->p->next_tx_package_id++;
	if (iccom->p->next_tx_package_id <= 0) {
		iccom->p->next_tx_package_id = ICCOM_INITIAL_PACKAGE_ID;
	}
	return pkg_id;
}

// Helper. Returns true if we have at least one package in TX list.
static inline bool __iccom_have_packages(struct iccom_dev *iccom)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("", return -EINVAL);
#endif
	return !list_empty(&iccom->p->tx_data_packages_head);
}

// Helper. Enqueues new empty (unfinalized) package to fill to tx
// packages queue (to the end of queue). The former last package
// (if one) will be finalized. The ID of the package is set upon
// creation.
//
// NOTE: The newly added package is not finalized, but is ready for data
//      to be added.
//
// RETURNS:
//      0 on success
//      < 0 - the negative error code
static int __iccom_enqueue_new_tx_data_package(struct iccom_dev *iccom)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("", return -EINVAL);
#endif
	if (__iccom_have_packages(iccom)) {
		__iccom_package_finalize(__iccom_get_last_tx_package(iccom));
	}

	struct iccom_package *new_package;
	new_package = kmalloc(sizeof(struct iccom_package), GFP_KERNEL);
	if (!new_package) {
		iccom_err("no memory for new package");
		return -ENOMEM;
	}

	int res = __iccom_package_init(new_package
				       , ICCOM_DATA_XFER_SIZE_BYTES);
	if (res < 0) {
		iccom_err("no memory for new package");
		kfree(new_package);
		return res;
	}

	int package_id = __iccom_get_next_package_id(iccom);
	__iccom_package_set_id(new_package, package_id);

	list_add_tail(&new_package->list_anchor
		      , &iccom->p->tx_data_packages_head);

	iccom->p->statistics.packages_in_tx_queue++;

	return 0;
}

// Helper. Returns true if we have > 1 packages in TX packages queue.
//
// LOCKING: storage should be locked before this call
//
// RETURNS:
//      true: if >1 packages exist in TX packages queue
//      false: else
static bool __iccom_have_multiple_packages(struct iccom_dev *iccom)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return false);
	ICCOM_CHECK_DEVICE_PRIVATE("", return false);
#endif
	struct list_head *const head = &iccom->p->tx_data_packages_head;
	struct list_head *curr = head;
	int count;
	for (count = 0; count < 2; count++) {
		if (curr->next == head) {
			break;
		}
		curr = curr->next;
	}
	return count == 2;
}

// Helper. Enqueues new finalized and empty package to tx packages
// queue (to the end of queue). The former last package (if one)
// is finalized.
//
// RETURNS:
//      0: on success
//      <0: negative error code on error
static int __iccom_enqueue_empty_tx_data_package(struct iccom_dev *iccom)
{
	int res = __iccom_enqueue_new_tx_data_package(iccom);
	if (res != 0) {
		return res;
	}
	struct iccom_package *pkg = __iccom_get_last_tx_package(iccom);
	__iccom_package_make_empty(pkg);
	return 0;
}

// Helper. Returns true if the package checksum is correct.
static inline bool __iccom_verify_package_crc(
	struct iccom_package *package)
{
	return __iccom_package_get_src(package)
		    == __iccom_package_compute_src(package);
}

// Helper. Verifies the selfconsistency of all package-level data
// (header, crc, free space).
//
// RETURNS:
//      package is ok: the package payload size >= 0
//      else: -1
static int __iccom_verify_package_data(struct iccom_package *package)
{
	bool pkg_ok;
	size_t payload_size = __iccom_package_payload_size(package, &pkg_ok);
	if (!pkg_ok) {
		iccom_info(ICCOM_LOG_INFO_DBG_LEVEL
			   , "RX Package PL size incorrect: %zu"
			   , payload_size);
		iccom_dbg_printout_package(package);
		return -1;
	}
	if (!__iccom_package_check_unused_payload(package
			, ICCOM_PACKAGE_EMPTY_PAYLOAD_VALUE)) {
		iccom_info(ICCOM_LOG_INFO_DBG_LEVEL
			    , "RX Package layout incorrect:"
			      " PL free space not filled with %hhx"
			    , ICCOM_PACKAGE_EMPTY_PAYLOAD_VALUE);
		iccom_dbg_printout_package(package);
		return -1;
	}
	if (!__iccom_verify_package_crc(package)) {
		iccom_info(ICCOM_LOG_INFO_DBG_LEVEL
			   , "RX Package CRC incorrect");
		iccom_dbg_printout_package(package);
		return -1;
	}
	return (int)payload_size;
}

// Helper. Fills up the full_duplex_xfer data structure to make a
// full-duplex data xfer for the first pending data package in TX queue.
//
// NOTE: surely the first package in TX queue should be finalized before
//      this call
static void __iccom_fillup_next_data_xfer(struct iccom_dev *iccom
					  , struct full_duplex_xfer *xfer)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return);
	if (IS_ERR_OR_NULL(xfer)) {
		iccom_err("Valid xfer provided. Logical error.");
		return;
	}
	if (!__iccom_have_packages(iccom)) {
		iccom_err("No packages in TX queue. Logical error.");
		return;
	}
#endif

	struct iccom_package *src_pkg = __iccom_get_first_tx_package(iccom);

#ifdef ICCOM_DEBUG
	if (IS_ERR_OR_NULL(src_pkg)) {
		iccom_err("Broken pkg pointer. Logical error.");
		return;
	}
	if (__iccom_verify_package_data(src_pkg) < 0) {
		iccom_err("First TX package is not finalized. Logical error.");
		return;
	}
#endif
	xfer->size_bytes = src_pkg->size;
	xfer->data_tx = src_pkg->data;
	xfer->data_rx_buf = NULL;
	xfer->consumer_data = (void*)iccom;
	xfer->done_callback = &__iccom_xfer_done_callback;
	xfer->fail_callback = &__iccom_xfer_failed_callback;
}

// Helper. Fills up the full_duplex_xfer data structure to make a
// full-duplex ack/nack xfer.
static inline void __iccom_fillup_ack_xfer(
		struct iccom_dev *iccom
		, struct full_duplex_xfer *xfer
		, bool ack)
{
	xfer->size_bytes = ICCOM_ACK_XFER_SIZE_BYTES;
	xfer->data_tx = ack ? &iccom->p->ack_val : &iccom->p->nack_val;
	xfer->data_rx_buf = NULL;
	xfer->consumer_data = (void*)iccom;
	xfer->done_callback = &__iccom_xfer_done_callback;
	xfer->fail_callback = &__iccom_xfer_failed_callback;
}

// Helper. Returns true if the package is ACK package which approves
// the correct receiving of the data.
static inline bool __iccom_verify_ack(struct iccom_package *package)
{
	return (package->size == ICCOM_ACK_XFER_SIZE_BYTES)
		&& (package->data[0] == ICCOM_PACKAGE_ACK_VALUE);
}

// Helper. Moves TX package queue one step forward.
// If there are multiple data packages, simply discards the heading
// package. If there is only one package (which is supposed to be
// just sent), then empties it, updates its ID and finalizes it so
// it is ready for next xfer.
//
// To be called when the first in TX queue package xfer was
// proven to be done successfully (its ACK was received).
//
// NOTE: thread safe
//
// RETURNS:
//      if there is a non-empty package for xfer from our side.
static bool __iccom_queue_step_forward(struct iccom_dev *iccom)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return false);
	ICCOM_CHECK_DEVICE_PRIVATE("", return false);
	if (!__iccom_have_packages(iccom)) {
		iccom_err("empty TX packages: logical error");
		return false;
	}
#endif
	bool have_data;
	// this function is called indirectly by transport layer below,
	// while the TX queue can be updated independently by the
	// consumer, so need mutex here for protection
	mutex_lock(&iccom->p->tx_queue_lock);

	// if we have more than one package in the queue, this
	// means we have some our data to send in further packages
	if (__iccom_have_multiple_packages(iccom)) {
		struct iccom_package *delivered_package
			     = __iccom_get_first_tx_package(iccom);
		__iccom_package_free(delivered_package);
		iccom->p->statistics.packages_in_tx_queue--;
		have_data = true;
		goto finalize;
	}

	// we have only one package in queue
	struct iccom_package *delivered_package
		= __iccom_get_first_tx_package(iccom);

	// set this package empty and update with new id
	int next_id = __iccom_get_next_package_id(iccom);
	__iccom_package_set_id(delivered_package, next_id);
	__iccom_package_make_empty(delivered_package);

	have_data = false;
finalize:
	mutex_unlock(&iccom->p->tx_queue_lock);
	return have_data;
}

// Frees whole TX queue (should be called only on ICCom
// destruction when all external calls which might modify the
// TX queue are already disabled).
static void __iccom_queue_free(struct iccom_dev *iccom)
{
#ifdef ICCOM_DEBUG
	if (!__iccom_have_packages(iccom)) {
	    iccom_err("empty TX packages");
	}
#endif
	mutex_lock(&iccom->p->tx_queue_lock);

	struct iccom_package *first_package
		    = __iccom_get_first_tx_package(iccom);

	while (first_package) {
	    __iccom_package_free(first_package);
	    first_package = __iccom_get_first_tx_package(iccom);
	}

	// freeing the TX queue access
	mutex_unlock(&iccom->p->tx_queue_lock);

	mutex_destroy(&iccom->p->tx_queue_lock);
}

// Helper. Enqueues given message into the queue. Adds as many
// packages as needed.
//
// CONCURRENCE: thread safe
//
// RETURNS:
//      < 0 : the negated error number
//      0   : success
static int __iccom_queue_append_message(struct iccom_dev *iccom
			       , char *data, const size_t length
			       , unsigned int channel
			       , unsigned int priority)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_CHANNEL("", return -EBADSLT);
	if (IS_ERR_OR_NULL(data)) {
		iccom_err("bad data pointer");
		return -EINVAL;
	}
	if (!length) {
		iccom_err("bad data length");
		return -ENODATA;
	}
	if (!__iccom_have_packages(iccom)) {
		iccom_err("empty TX packages: logical error");
		return -EFAULT;
	}
#endif

	int bytes_written = 0;
	struct iccom_package *dst_package = NULL;
	int res = 0;

	mutex_lock(&iccom->p->tx_queue_lock);
	// we will assume the first package to be in active xfer
	// (however there might be some IDLE pause between xfers)
	// so if only one package left we will simply add a brand
	// new one
	if (!__iccom_have_multiple_packages(iccom)) {
		// TODO: we can consider to update first package
		// if its xfer have not yet began, to do this we need
		// to create its updated memory in separate place,
		// and then try to update the xfer data on transport
		// layer device, and if succeeded, then we may put
		// this data into the first package.
		res = __iccom_enqueue_new_tx_data_package(iccom);
		if (res < 0) {
			iccom_err("Could not post message: err %d", res);
			goto finalize;
		}
	}

	while (length > bytes_written) {
		dst_package = __iccom_get_last_tx_package(iccom);

		// adding message part (or whole) to the latest package
		int bytes_written_old = bytes_written;
		bytes_written += iccom_package_add_packet(
					 dst_package
					 , data + bytes_written
					 , length - bytes_written
					 , channel);

		// some bytes were written to the package
		if (bytes_written != bytes_written_old) {
			continue;
		}

		// last package in queue already has no space
		res = __iccom_enqueue_new_tx_data_package(iccom);
		if (res < 0) {
			// TODO make robust previous packages
			//      cleanup here to remove parts of
			//      failed message.
			iccom_err("Could not post message: err %d"
				  , res);
			goto finalize;
		}
	}

	// we will always finalize package to make it always be ready
	// for sending, however this doesn't mean that we can not add
	// more data to the finalized but not full package later
	__iccom_package_finalize(dst_package);

finalize:
	mutex_unlock(&iccom->p->tx_queue_lock);
	return res;
}

// Helper. Adds new message to the storage channel. If channel does
// not exist, creates it. Returns pointer to newly created message.
//
// @iccom {valid iccom pointer}
// @channel {valid channel number} the channel to add the new message
//
// CONCURRENCE: thread safe
// OWNERSHIP: of the new message belongs to the storage
//
// RETURNS:
//      !NULL: valid pointer to newly created and initialized
//          iccom_message
//      NULL: if fails
static struct iccom_message *__iccom_construct_message_in_storage(
		struct iccom_dev *iccom
		, unsigned int channel)
{
	struct iccom_message *msg;
	msg = kmalloc(sizeof(struct iccom_message), GFP_KERNEL);
	if (IS_ERR_OR_NULL(msg)) {
		iccom_err("No memory for new message");
		return NULL;
	}
	// TODO: allocate either the message maximum or expected
	// (if known) size to avoid reallocation
	__iccom_message_init(msg);
	msg->channel = channel;

	if (iccom_msg_storage_push_message(&iccom->p->rx_messages
					   , msg) != 0) {
		kfree(msg);
		return NULL;
	}
	return msg;
}

// Helper. Parses the next packet from the package. Starts at given
// position and if parsing is successful adds the parsed consumer data
// into the iccom consumer messages storage.
//
// @iccom {valid iccom ptr}
// @start_from {valid ptr} the pointer to the first packet byte
//      (first byte of packet header)
// @max_bytes_available the maximum possible total packet size (in
//      bytes), this is usually equal to the number of bytes left till
//      the end of the package payload area.
//      NOTE: any value less than minimal possible packet size
//          immediately will lead to parsing error.
// @consumer_bytes_count__out {NULL | valid ptr}: pointer to the
//      output variable where to ADD number of consumer bytes
//      parced from the packet. If not valid ptr - not used.
// @finalized_message__out {NULL | valid ptr}: pointer to the
//      output variable where to WRITE, if the message was just
//      finalized. If not valid ptr - not used.
//
// NOTE: if parsing of a packet failed, then all rest packets from
//      given package will be dropped, as long as the parsing
//      will be unreliable.
//
// CONCURRENCE: no simultaneous calls allowed, but finalized messages
//      storage can be read by consumer freely
//
// RETURNS:
//      >0: size of data read from the start (equals to the size of the
//          package which was read (in bytes)), this also means that
//          parsing of the packet was successful.
//       0: caller provided 0 bytes available, so nothing left to parse
//      <0: negative error number if parsing failed
static int __iccom_read_next_packet(struct iccom_dev __kernel *iccom
	, void __kernel *start_from
	, size_t max_bytes_available
	, size_t *consumer_bytes_count__out
	, bool *finalized_message__out)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("", return -EINVAL);
	ICCOM_CHECK_PTR(start_from, return -EINVAL);
#endif
	if (max_bytes_available == 0) {
		return 0;
	}

	struct iccom_packet packet;
	int res = __iccom_packet_parse_into_struct(start_from
				    , max_bytes_available, &packet);
	if (res < 0) {
		iccom_err("Broken packet detected.");
		return res;
	}

	// while message id is not used we always append data to the
	// latest message within the channel
	// TODO: adapt as protocol includes message ID
	struct iccom_message *msg
			= iccom_msg_storage_get_last_unfinalized_message(
					    &iccom->p->rx_messages
					    , packet.channel);

	if (!msg) {
		msg = __iccom_construct_message_in_storage(
				iccom, packet.channel);
		if (!msg) {
			iccom_err("No memory for incoming message.");
			return -ENOMEM;
		}
	}

	res = iccom_msg_storage_append_data_to_message(
		    &iccom->p->rx_messages, msg->channel, msg->id
		    , packet.payload, packet.payload_length
		    , packet.finalizing);

	if (res < 0) {
		return res;
	}
	if (!IS_ERR_OR_NULL(consumer_bytes_count__out)) {
		*consumer_bytes_count__out = packet.payload_length;
	}
	if (!IS_ERR_OR_NULL(finalized_message__out)) {
		*finalized_message__out = packet.finalizing;
	}

	return iccom_packet_packet_size_bytes(packet.payload_length);
}

// Helper. Full parsing of the package and dispatching all its packets
// to the consumer messages storage.
//
// @iccom {valid iccom ptr}
// @start_from {valid ptr} to the first byte of the package
//      payload.
// @payload_size {>=0} the exact size of the package payload in bytes
//
// Rolls back the applied changes from the package if parsing
// fails at some point. So the message storage is guaranteed to
// remain in selfconsistent state: either whole package is
// applied or whole package is not applied.
//
// To be called directly or indirectly by transport layer.
//
// CONCURRENCE: no simultaneous calls allowed, protected
//      async reading of finalized messages is OK.
//
// RETURNS:
//      0: on successful parsing of the whole package
//      -EBADMSG: if parsing failed (also due to out-of-memory conditions)
static int __iccom_process_package_payload(
		struct iccom_dev __kernel *iccom
		, void __kernel *start_from
		, size_t payload_size)
{
	int packets_done = 0;
	size_t bytes_to_parse = payload_size;
	void *start = start_from;
	size_t consumer_bytes_parsed_total = 0;

	while (bytes_to_parse > 0) {
		int bytes_read = __iccom_read_next_packet(iccom
					    , start, bytes_to_parse
					    , &consumer_bytes_parsed_total
					    , NULL);
		if (bytes_read <= 0) {
			iccom_msg_storage_rollback(&iccom->p->rx_messages);
			iccom_err("Package parsing failed on %d packet"
				  "(starting from 0). Error code: %d"
				  , packets_done, bytes_read);
			print_hex_dump(KERN_WARNING, ICCOM_LOG_PREFIX
				       ": Failed package payload: ", 0, 16, 1
				       , start_from, payload_size, true);
			// NOTE: the no-memory case is aggregated here also
			// 	to ask other size to resend the message
			return -EBADMSG;
		}

		start += bytes_read;
		bytes_to_parse -= bytes_read;
		packets_done++;
	}

	int finalized = iccom_msg_storage_uncommitted_finalized_count(
				&iccom->p->rx_messages);
	iccom_msg_storage_commit(&iccom->p->rx_messages);

	iccom->p->statistics.packets_received_ok += packets_done;
	iccom->p->statistics.messages_received_ok += finalized;
	iccom->p->statistics.total_consumers_bytes_received_ok
			+= consumer_bytes_parsed_total;
	__sync_add_and_fetch(
			&iccom->p->statistics.messages_ready_in_storage
			, finalized);

	if (finalized > 0) {
		// notify consumer if there is any new ready messages
		__iccom_schedule_work(iccom
				, &iccom->p->consumer_delivery_work);
	}
	return 0;
}

// Helper. Initiates the xfer of the first package in TX queue using
// the underlying transport layer. We must have finalized data package
// in TX queue before calling this function.
//
// NOTE: if the underlying transport is busy, then we will not shedule
//      xfer here, but in xfer-done callback.
//
// RETURNS:
//      0 on success
//      < 0 - the negative error code
static int __iccom_initiate_data_xfer(struct iccom_dev *iccom)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("", return -EINVAL);
#endif
	if (!__iccom_have_packages(iccom)) {
		iccom_err("No data to be sent.");
		return -ENODATA;
	}

	// to guarantee selfconsistence, we will just trigger
	// current xfer, while the xfer data is only to be updated
	// from the xfer done callback, the xfer done callback is
	// guaranteed to be called after data_xchange(...) invocation
	int res = iccom->xfer_iface.data_xchange(
			iccom->xfer_device, NULL, false);

	switch (res) {
	case FULL_DUPLEX_ERROR_NOT_READY:
		return 0;
	case FULL_DUPLEX_ERROR_NO_DEVICE_PROVIDED:
		iccom_err("No underlying xfer device provided");
		return -ENODEV;
	default: return 0;
	}
}


// Transport layer return point.
//
// Called from transport layer, when xfer failed. Dedicated
// to handle supervised error recovery or halting xfer device.
//
// See full_duplex_xfer.fail_callback description for details.
//
// CONCURRENCE: no simultaneous calls, also with other
//      transport layer return points
struct full_duplex_xfer *__iccom_xfer_failed_callback(
		const struct full_duplex_xfer __kernel *failed_xfer
		, const int next_xfer_id
		, int error_code
		, void __kernel *consumer_data)
{
	// The xfer failed

	struct iccom_dev *iccom = (struct iccom_dev *)consumer_data;
	ICCOM_CHECK_DEVICE("External error. No device provided."
			   , return ERR_PTR(-ENODATA));
	// if we are closing, then we will halt bottom transport layer
	// by returning error pointer value
	ICCOM_CHECK_CLOSING("will not invoke", return ERR_PTR(-ENODATA));

	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL, "FAILED xfer:");
	iccom_dbg_printout_xfer(failed_xfer);
	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL, "TX queue:");
	iccom_dbg_printout_tx_queue(iccom
				, ICCOM_DEBUG_PACKAGES_PRINT_MAX_COUNT);

	__iccom_err_report(ICCOM_ERROR_TRANSPORT, error_code);

	// we always goto ack stage with NACK package
	// and then repeat the data xfer within the next frame.
	__iccom_fillup_ack_xfer(iccom, &iccom->p->xfer, false);
	iccom->p->data_xfer_stage = false;

	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL, "Next xfer:");
	iccom_dbg_printout_xfer(&iccom->p->xfer);
	return &iccom->p->xfer;
}

// Transport layer return point.
//
// CONCURRENCE: no simultaneous calls
//
// Called from transport layer, when xfer is done.
struct full_duplex_xfer *__iccom_xfer_done_callback(
			const struct full_duplex_xfer __kernel *done_xfer
			, const int next_xfer_id
			, bool __kernel *start_immediately__out
			, void *consumer_data)
{
	// The xfer was just finished. The done_xfer.data_rx_buf contains
	// the received data.

	struct iccom_dev *iccom = (struct iccom_dev *)consumer_data;
	ICCOM_CHECK_DEVICE("External error. No device provided."
			   , return ERR_PTR(-ENODATA));
	// if we are closing, then we will halt bottom transport layer
	// by returning error pointer value
	ICCOM_CHECK_CLOSING("will not invoke", return ERR_PTR(-ENODATA));

	// convenience wrappers around done_xfer.data_tx/rx_buf
	struct iccom_package rx_pkg = {.data = done_xfer->data_rx_buf
				       , .size = done_xfer->size_bytes
				       , .owns_data = false};
	if (IS_ERR_OR_NULL(rx_pkg.data)) {
		iccom_err("got broken RX data pointer: %px; "
			  , rx_pkg.data);
		return ERR_PTR(-ENODATA);
	}

	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL, "Done xfer:");
	iccom_dbg_printout_xfer(done_xfer);
	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL, "TX queue:");
	iccom_dbg_printout_tx_queue(iccom
			, ICCOM_DEBUG_PACKAGES_PRINT_MAX_COUNT);

	iccom->p->statistics.raw_bytes_xfered_via_transport_layer
		    += done_xfer->size_bytes;
	iccom->p->statistics.transport_layer_xfers_done_count++;

	// If we are in data xfering stage, thus the data xfer has been
	// just finished, so we need to verify it and send the ack/nack
	// answer back. TODO: later we may indicate the CRC failure with
	// other side drop flag timeout so we'll need no ack/nack xfers
	// at all.
	if (iccom->p->data_xfer_stage) {
		iccom->p->statistics.packages_xfered++;

		*start_immediately__out = true;

		int payload_size = __iccom_verify_package_data(&rx_pkg);

		// if package level data is not selfconsistent
		if (payload_size < 0) {
			__iccom_fillup_ack_xfer(iccom, &iccom->p->xfer, false);
			iccom->p->statistics.packages_bad_data_received += 1;
			goto finalize;
		}

		// package is selfconsistent, but we already received
		// and processed it successfully, then we will say that
		// this package is (already) OK
		int rx_pkg_id = __iccom_package_get_id(&rx_pkg);
		if (rx_pkg_id == iccom->p->last_rx_package_id) {
			__iccom_fillup_ack_xfer(iccom, &iccom->p->xfer, true);
			iccom->p->statistics.packages_duplicated_received += 1;
			goto finalize;
		}

		// package is selfconsistent and we have not processed it
		// yet, so we'll try to process it
		void *pkg_payload = __iccom_package_payload_start_addr(&rx_pkg);
		if (__iccom_process_package_payload(iccom, pkg_payload
					  , (size_t)payload_size) != 0) {
			__iccom_fillup_ack_xfer(iccom, &iccom->p->xfer, false);
			iccom->p->statistics.packages_parsing_failed += 1;
			goto finalize;
		}

		// package parsing was OK
		iccom->p->statistics.packages_received_ok++;
		iccom->p->last_rx_package_id = rx_pkg_id;
		__iccom_fillup_ack_xfer(iccom, &iccom->p->xfer, true);
		goto finalize;
	}

	// If we are in ack stage, then we have just finished the
	// ack xfer and can goto to the next frame (using old or
	// new data depending on the ack state of the other side).

	// If other side acked the correct receiving of our data
	if (__iccom_verify_ack(&rx_pkg)) {
		iccom->p->statistics.packages_sent_ok++;
		// TODO to schedule only if at least one message finalized
		*start_immediately__out = __iccom_queue_step_forward(iccom);
	} else {
		// We must resend the failed package immediately.
		// TODO: probably we may avoid resending the empty
		//      package if new packages arrived in TX queue.
		*start_immediately__out = true;
	}

	// preparing the next xfer with the first pending package in queue
	__iccom_fillup_next_data_xfer(iccom, &iccom->p->xfer);

finalize:
	// switching to other stage (the only point where the
	// data_xfer_stage is being written)
	iccom->p->data_xfer_stage = !iccom->p->data_xfer_stage;

#ifdef ICCOM_DEBUG
	mutex_lock(&iccom->p->rx_messages.lock);
	__iccom_msg_storage_printout(&iccom->p->rx_messages
				     , ICCOM_DEBUG_MESSAGES_PRINTOUT_MAX_COUNT
				     , ICCOM_DEBUG_CHANNEL);
	mutex_unlock(&iccom->p->rx_messages.lock);
#endif
	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL, "Next xfer:");
	iccom_dbg_printout_xfer(&iccom->p->xfer);

	return &iccom->p->xfer;
}

// Stops the underlying byte xfer device and detaches it from
// the ICCom driver.
void __iccomm_stop_xfer_device(struct iccom_dev *iccom)
{
	iccom->xfer_iface.close(iccom->xfer_device);
}


// The consumer notification procedure, is sheduled every time, when
// any incoming message is finished and ready to be delivered to the
// consumer. Due to latency in scheduling not only notifies about
// initial message, but also checks for other finished messages.
//
// @work the scheduled work which launched the notification
static void __iccom_consumer_notification_routine(
	struct work_struct *work)
{
	if (IS_ERR_OR_NULL(work)) {
		iccom_err("no notification work provided");
		return;
	}

	struct iccom_dev_private *iccom_p = container_of(work
		, struct iccom_dev_private, consumer_delivery_work);

	int passed = iccom_msg_storage_pass_ready_data_to_consumer(
					     &iccom_p->rx_messages);
	if (passed >= 0) {
		__sync_add_and_fetch(
			    &iccom_p->statistics.messages_ready_in_storage
			    , -passed);
	}
}

// Verifies if the interface of full duplex transport device contains all
// records relevant for ICCom
static bool __iccom_verify_transport_layer_interface(
		const struct full_duplex_sym_iface  *const iface)
{
	return !IS_ERR_OR_NULL(iface)
		    && !IS_ERR_OR_NULL(iface->data_xchange)
		    && !IS_ERR_OR_NULL(iface->is_running)
		    && !IS_ERR_OR_NULL(iface->init)
		    && !IS_ERR_OR_NULL(iface->reset)
		    && !IS_ERR_OR_NULL(iface->close);
}

// Helper. Initializes the iccom packages storage (TX storage).
// RETURNS:
//      0: all fine
//      <0: negative error code
static inline int __iccom_init_packages_storage(struct iccom_dev *iccom)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("", return -EINVAL);
#endif
	INIT_LIST_HEAD(&iccom->p->tx_data_packages_head);
	mutex_init(&iccom->p->tx_queue_lock);
	iccom->p->next_tx_package_id = ICCOM_INITIAL_PACKAGE_ID;
	return 0;
}

// Helper. Frees all resources owned by packages storage.
static inline void __iccom_free_packages_storage(struct iccom_dev *iccom)
{
#ifdef ICCOM_DEBUG
	ICCOM_CHECK_DEVICE("", return);
	ICCOM_CHECK_DEVICE_PRIVATE("", return);
#endif
	mutex_lock(&iccom->p->tx_queue_lock);
	while (__iccom_have_packages(iccom)) {
		__iccom_package_free(__iccom_get_first_tx_package(iccom));
	}
	mutex_unlock(&iccom->p->tx_queue_lock);
	mutex_destroy(&iccom->p->tx_queue_lock);
}

// Helper. Inits the ICCom procfs.
// RETURNS:
//      >= 0: on success,
//      < 0: on failure (negated error code)
static inline int __iccom_procfs_init(struct iccom_dev *iccom)
{
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("", return -ENODEV);

	iccom->p->proc_root = proc_mkdir(ICCOM_PROC_ROOT_NAME, NULL);

	if (IS_ERR_OR_NULL(iccom->p->proc_root)) {
		iccom_err("failed to create ICCom proc root folder"
			  " with name: "ICCOM_PROC_ROOT_NAME);
		return -EIO;
	}
	return 0;
}

// Removes the ICcom proc root
static void __iccom_procfs_close(struct iccom_dev *iccom)
{
	ICCOM_CHECK_DEVICE("", return);
	ICCOM_CHECK_DEVICE_PRIVATE("", return);

	if (IS_ERR_OR_NULL(iccom->p->proc_root)) {
		return;
	}

	proc_remove(iccom->p->proc_root);
	iccom->p->proc_root = NULL;
}


// Helper. Initializes the statistics structure of ICCom.
// NOTE: the ICCom proc rootfs should be created beforehand,
//      if not: then we will fail to create statistics node.
//
// RETURNS:
//      >= 0: on success,
//      < 0: on failure (negated error code)
static inline int __iccom_statistics_init(struct iccom_dev *iccom)
{
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("", return -ENODEV);

	// initial statistics data
	memset(&iccom->p->statistics, 0, sizeof(iccom->p->statistics));

	// statistics access operations
	memset(&iccom->p->statistics_ops, 0, sizeof(iccom->p->statistics_ops));
	iccom->p->statistics_ops.read  = &__iccom_statistics_read;
	iccom->p->statistics_ops.owner = THIS_MODULE;

	if (IS_ERR_OR_NULL(iccom->p->proc_root)) {
		iccom_err("failed to create statistics proc entry:"
			  " no ICCom root proc entry");
		iccom->p->statistics_file = NULL;
		return -ENOENT;
	}

	iccom->p->statistics_file = proc_create_data(
					   ICCOM_STATISTICS_FILE_NAME
					   , ICCOM_PROC_R_PERMISSIONS
					   , iccom->p->proc_root
					   , &iccom->p->statistics_ops
					   , (void*)iccom);

	if (IS_ERR_OR_NULL(iccom->p->statistics_file)) {
		iccom_err("failed to create statistics proc entry.");
		return -EIO;
	}

	return 0;
}

// Removes the ICcom proc statistics file
static void __iccom_statistics_close(struct iccom_dev *iccom)
{
	ICCOM_CHECK_DEVICE("", return);
	ICCOM_CHECK_DEVICE_PRIVATE("", return);

	if (IS_ERR_OR_NULL(iccom->p->statistics_file)) {
		return;
	}

	proc_remove(iccom->p->statistics_file);
	iccom->p->statistics_file = NULL;
}

// Provides the read method for ICCom statistics to user world.
// Is invoked when user reads the /proc/<ICCOM>/<STATISTICS> file.
//
// Is restricted to the file size of SIZE_MAX bytes.
//
// RETURNS:
//      >= 0: number of bytes actually provided to user space, on success
//      < 0: negated error code, on failure
//
static ssize_t __iccom_statistics_read(struct file *file
		, char __user *ubuf
		, size_t count
		, loff_t *ppos)
{
	ICCOM_CHECK_PTR(file, return -EINVAL);
	ICCOM_CHECK_PTR(ubuf, return -EINVAL);
	ICCOM_CHECK_PTR(ppos, return -EINVAL);

	struct iccom_dev *iccom = (struct iccom_dev *)PDE_DATA(file->f_inode);

	ICCOM_CHECK_DEVICE("no device provided", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("broken device data", return -ENODEV);

	const int BUFFER_SIZE = 2048;

	if (*ppos >= BUFFER_SIZE || *ppos > SIZE_MAX) {
		return 0;
	}

	char *buf = kmalloc(BUFFER_SIZE, GFP_KERNEL);

	if (IS_ERR_OR_NULL(buf)) {
		return -ENOMEM;
	}

	const struct iccom_dev_statistics * const s = &iccom->p->statistics;
	size_t len = (size_t)snprintf(buf, BUFFER_SIZE
		     , "transport_layer: xfers done:  %llu\n"
		       "transport_layer: bytes xfered:  %llu\n"
		       "packages: xfered total:  %llu\n"
		       "packages: sent ok:  %llu\n"
		       "packages: received ok:  %llu\n"
		       "packages: sent fail (total):  %llu\n"
		       "packages: received fail (total):  %llu\n"
		       "packages:     received corrupted:  %llu\n"
		       "packages:     received duplicated:  %llu\n"
		       "packages:     detailed parsing failed:  %llu\n"
		       "packages: in tx queue:  %lu\n"
		       "packets: received ok:  %llu\n"
		       "messages: received ok:  %llu\n"
		       "messages: ready rx:  %lu\n"
		       "bandwidth: consumer bytes received:\t%llu\n"
		       "\n"
		       "Note: this is only general statistical/monitoring"
		       " info and is not expeted to be used in precise"
		       " measurements due to atomic selfconsistency"
		       " maintenance would put overhead in the driver.\n"
		     , s->transport_layer_xfers_done_count
		     , s->raw_bytes_xfered_via_transport_layer
		     , s->packages_xfered
		     , s->packages_sent_ok
		     , s->packages_received_ok
		     , s->packages_xfered - s->packages_sent_ok
		     , s->packages_xfered - s->packages_received_ok
		     , s->packages_bad_data_received
		     , s->packages_duplicated_received
		     , s->packages_parsing_failed
		     , s->packages_in_tx_queue
		     , s->packets_received_ok
		     , s->messages_received_ok
		     , s->messages_ready_in_storage
		     , s->total_consumers_bytes_received_ok);
	len++;

	if (len > BUFFER_SIZE) {
		iccom_warning("statistics output was too big for buffer"
			      ", required length: %zu", len);
		len = BUFFER_SIZE;
		buf[BUFFER_SIZE - 1] = 0;
	}

	const unsigned long nbytes_to_copy
			= (len >= (size_t)(*ppos))
				?  min(len - (size_t)(*ppos), count)
				: 0;
	const unsigned long not_copied
			= copy_to_user(ubuf, buf + (size_t)(*ppos)
				       , nbytes_to_copy);
	kfree(buf);
	buf = NULL;
	*ppos += nbytes_to_copy - not_copied;

	return nbytes_to_copy - not_copied;
}

/* -------------------------- KERNEL SPACE API --------------------------*/

// API
//
// Sends the consumer data to the other side via specified channel.
//
// @data {valid data pointer} the consumer data to be sent to external
//      @channel.
//      NOTE: consumer guarantees that the data remain untouched
//              until call returns.
//      OWNERSHIP:
//              consumer
// @length {>0} the @data size in bytes.
// @channel [ICCOM_PACKET_MIN_CHANNEL_ID; ICCOM_PACKET_MAX_CHANNEL_ID]
//      the id of the channel to be used to send the message
//      should be between ICCOM_PACKET_MIN_CHANNEL_ID and
//      ICCOM_PACKET_MAX_CHANNEL_ID inclusive
// @priority Defines the message priority. TODO (not yet implemented).
// @iccom {valid iccom device ptr} the protocol driver to be used to
//      send the message
//
// CONCURRENCE: thread safe
//
// RETURNS:
//      0 : on success
//
//      TODO:
//          Message id (>= 0) on success (it can be used as timestamp
//          the bigger the id the later message was ordered for xfer).
//
//      <0 : negated error code if fails.
__maybe_unused
int iccom_post_message(struct iccom_dev *iccom
		, char *data, const size_t length
		, unsigned int channel
		, unsigned int priority)
{
	ICCOM_CHECK_DEVICE("no device provided", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("broken device data", return -EINVAL);
	ICCOM_CHECK_CHANNEL("bad channel", return -EBADSLT);
	if (IS_ERR_OR_NULL(data)) {
		iccom_err("broken data pointer provided");
		return -EINVAL;
	}
	if (!length) {
		iccom_err("Will not post empty message.");
		return -ENODATA;
	}
	ICCOM_CHECK_CLOSING("will not invoke", return -EBADFD);

#if defined(ICCOM_DEBUG) && defined(ICCOM_DEBUG_PACKAGES_PRINT_MAX_COUNT)
#if ICCOM_DEBUG_PACKAGES_PRINT_MAX_COUNT != 0
	iccom_info(ICCOM_LOG_INFO_DBG_LEVEL
		   , "TX queue before adding new message");
	mutex_lock(&iccom->p->tx_queue_lock);
	iccom_dbg_printout_tx_queue(iccom
			, ICCOM_DEBUG_PACKAGES_PRINT_MAX_COUNT);
	mutex_unlock(&iccom->p->tx_queue_lock);
#endif
#endif

	// TODO drop request if there are too many pending
	// packages to be sent.

	int res = 0;
	res = __iccom_queue_append_message(iccom, data, length, channel
					   , priority);

	if (res < 0) {
		iccom_err("Failed to post the message: err = %d", res);
		return res;
	}

	// for now we send the package if it is not empty
	res = __iccom_initiate_data_xfer(iccom);
	if (res < 0) {
		iccom_err("Failed to post the message: err = %d", res);
		return res;
	}

	return 0;
}

// API
//
// Forces the ICCom to start xfer of the current heading package
// even it is empty.
//
// @iccom {valid iccom device ptr} the protocol driver to be used
//
// RETURNS:
//      0 : on success
//
//      TODO:
//          Message id (>= 0) on success (it can be used as timestamp
//          the bigger the id the later message was ordered for xfer).
//
//      <0 : negated error code if fails.
__maybe_unused
int iccom_flush(struct iccom_dev *iccom)
{
	int res = __iccom_initiate_data_xfer(iccom);
	if (res < 0) {
		iccom_err("Failed to initiate the message: err = %d", res);
		return res;
	}

	return 0;
}

// API
//
// Adds the message ready callback to the channel. This callback will be
// called every time a message is ready in the channel. After the callback
// invocation the message data is discarded.
//
// @iccom {valid ptr} device to work with
// @channel {valid channel number | ICCOM_ANY_CHANNEL_VALUE}
//      the channel to install the callback; if equals to
//      ICCOM_ANY_CHANNEL_VALUE then callback is installed
//      as global callback for all channels.
//      NOTE: global callback is used only when specific channel
//          callback is not set.
// @message_ready_callback {valid ptr || NULL} the pointer to the
//      callback which is to be called when channel gets a message
//      ready. NULL value disables the callback function on the channel.
//
//      CALLBACK DATA OWNERSHIP: if callback returns true, then
//          ownership of message data (@msg_data) is transferred to
//          the consumer; if callback returns false, then message
//          data ownership remains in ICCom, and the message (and its
//          data) is immediately discarded after callback invocation.
//
// @consumer_data {any} the consumer value to pass to the
//      @message_ready_callback.
//
// RETURNS:
//      0: on success
//      <0: negated error code
__maybe_unused
int iccom_set_channel_callback(struct iccom_dev *iccom
		, unsigned int channel
		, iccom_msg_ready_callback_ptr_t message_ready_callback
		, void *consumer_data)
{
	ICCOM_CHECK_DEVICE("no device provided", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("broken device data", return -EINVAL);
	ICCOM_CHECK_CHANNEL("bad channel", return -EBADSLT);
	ICCOM_CHECK_CLOSING("will not invoke", return -EBADFD);
	if (IS_ERR(message_ready_callback)) {
		iccom_err("broken callback pointer provided");
		return -EINVAL;
	}

	return iccom_msg_storage_set_channel_callback(
			&iccom->p->rx_messages, channel
			, message_ready_callback
			, consumer_data);
}

// API
//
// Removes callback from the channel.
//
// @iccom {valid ptr} device to work with
// @channel {valid channel number | ICCOM_ANY_CHANNEL_VALUE}
//      the channel to remove the callback; if equals to
//      ICCOM_ANY_CHANNEL_VALUE then global callback is
//      removed;
//      NOTE: if no callback defined for the channel,
//          its messages are simply discarded.
//
// RETURNS:
//      0: on success
//      <0: negated error code
__maybe_unused
int iccom_remove_channel_callback(struct iccom_dev *iccom
		, unsigned int channel)
{
	ICCOM_CHECK_DEVICE("no device provided", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("broken device data", return -EINVAL);
	ICCOM_CHECK_CHANNEL("bad channel", return -EBADSLT);
	ICCOM_CHECK_CLOSING("will not invoke", return -EBADFD);
	return iccom_msg_storage_reset_channel_callback(
			&iccom->p->rx_messages, channel);
}

// API
//
// Gets message ready callback of the channel.
//
// @iccom {valid ptr} device to work with
// @channel {valid channel number | ICCOM_ANY_CHANNEL_VALUE}
//      the channel to get the callback from; if equals to
//      ICCOM_ANY_CHANNEL_VALUE then global callback pointer
//      is returned.
//
// CONCURRENCE: thread safe
//
// RETURNS:
//      ERR PTR: on failure
//      NULL: if channel doesn't exist || callback is not set
//      callback pointer: if channel exists and callback is set
__maybe_unused
iccom_msg_ready_callback_ptr_t iccom_get_channel_callback(
		struct iccom_dev *iccom
		, unsigned int channel)
{
	ICCOM_CHECK_DEVICE("no device provided", return ERR_PTR(-ENODEV));
	ICCOM_CHECK_DEVICE_PRIVATE("broken device data", return ERR_PTR(-EINVAL));
	ICCOM_CHECK_CHANNEL("bad channel", return ERR_PTR(-EBADSLT));
	ICCOM_CHECK_CLOSING("will not invoke", return ERR_PTR(-EBADFD));
	return iccom_msg_storage_get_channel_callback(
			&iccom->p->rx_messages, channel);
}

// API
//
// Pops the oldest (first came) finalized and committed message
// from RX queue of the given channel to the kernel space.
//
// @iccom {inited iccom_dev ptr || ERR_PTR || NULL} the initialized ICCom
//      device pointer, if error-ptr or NULL, the error is returned.
// @channel {number} the channel number to read the message from.
//      If channel number is invalid then indicate it with error.
// @msg_data_ptr__out {valid kernel ptr || ERR_PTR || NULL}:OUT
//      the pointer to the pointer to the message data,
//      valid ptr:
//          set to:
//          NULL: if no message data provided (no messages available)
//          !NULL: pointer to message data (if message was get)
//          UNTOUCHED: if errors
//      ERR_PTR || NULL:
//          call does nothing, error returned
// @buf_size__out {valid kernel ptr || ERR_PTR || NULL}:OUT
//      the pointer to the size of the message data
//      valid ptr:
//          set to:
//          0: when *msg_data_ptr__out is set to NULL
//          >0: size of message data pointed by *msg_data_ptr__out
//              is !NULL
//          UNTOUCHED: if errors
//      ERR_PTR || NULL:
//          call does nothing, error returned
// @msg_id__out {valid kernel ptr || ERR_PTR || NULL}:OUT
//      the ptr to write the msg_id to.
//      valid ptr:
//          set to ICCOM_PACKET_INVALID_MESSAGE_ID if no message,
//          set to message ID if message is read,
//          untouched if other errors
//      ERROR/NULL: not used
//
// CONCURRENCE: thread safe
// OWNERSHIP: of the provided message data is transferred to the caller.
//
// RETURNS:
//     0: no errors (absence of messages is not an error)
//     negated error code if read failed:
//         -ENODEV: iccom_dev pointer is not valid
//         -EINVAL: iccom_dev pointer points to broken device
//         -EBADSLT: the channel provided is invalid (out of channel
//              range
//         -EFAULT: @msg_data_ptr__out or @buf_size__out pointer
//              is not valid
//         -EBADFD: the device is closing now, no calls possible
__maybe_unused
int iccom_read_message(struct iccom_dev *iccom
		, unsigned int channel
		, void __kernel **msg_data_ptr__out
		, size_t __kernel *buf_size__out
		, unsigned int *msg_id__out)
{
	ICCOM_CHECK_DEVICE("", return -ENODEV);
	ICCOM_CHECK_DEVICE_PRIVATE("", return -EINVAL);
	ICCOM_CHECK_CHANNEL("", return -EBADSLT);
	ICCOM_CHECK_CLOSING("", return -EBADFD);
	ICCOM_CHECK_PTR(msg_data_ptr__out, return -EFAULT);
	ICCOM_CHECK_PTR(buf_size__out, return -EFAULT);

	struct iccom_message *msg;
	msg = iccom_msg_storage_pop_first_ready_message(
			&iccom->p->rx_messages, channel);

	if (!msg) {
		*msg_data_ptr__out = NULL;
		*buf_size__out = 0;
		if (!IS_ERR_OR_NULL(msg_id__out)) {
			*msg_id__out = ICCOM_PACKET_INVALID_MESSAGE_ID;
		}
		return 0;
	}

	__sync_add_and_fetch(
		    &iccom->p->statistics.messages_ready_in_storage, -1);

	*msg_data_ptr__out = msg->data;
	*buf_size__out = msg->length;
	if (!IS_ERR_OR_NULL(msg_id__out)) {
		*msg_id__out = msg->id;
	}

	kfree(msg);
	return 0;
}

// API
//
// Initializes the iccom_dev structure.
//
// @iccom {valid iccom_dev ptr} managed by consumer. Not to be
//      amended while ICCom is active (not closed).
//
//      @xfer_device field of iccom_dev structure must
//      point to valid transport layer device.
//
//      @xfer_iface member should be valid and contain all
//      pointers.
//
//      iccom_dev_private structure pointer initialized by iccom
//      for internal needs.
//
// If this call succeeds, it is possible to use all other iccom
// methods on initialized iccom struct.
//
// NOTE: caller should never invoke ICCom methods on struct iccom_dev
// which init method didn't return with success state (yet).
//
// CONCURRENCE: caller should ensure that no one of iccom_init(...),
//      iccom_close(...) will be called under data-race conditions
//      with the same struct iccom_dev.
//
// CONTEXT: sleepable
//
// RETURNS:
//      0 on success
//      negative error code on error
__maybe_unused
int iccom_init(struct iccom_dev *iccom)
{
	ICCOM_CHECK_DEVICE("struct ptr broken", return -ENODEV);

	if (IS_ERR_OR_NULL(iccom->xfer_device)) {
		iccom_err("No transport layer device provided");
		return -ENODEV;
	}
	if (!__iccom_verify_transport_layer_interface(
			    &iccom->xfer_iface)) {
		iccom_err("Not all relevant interface methods are defined");
		return -ENODEV;
	}

	iccom_info_raw(ICCOM_LOG_INFO_OPT_LEVEL
		       , "creating device (%px)", iccom);

	// initialization sequence
	int res = 0;
	iccom->p = kmalloc(sizeof(struct iccom_dev_private), GFP_KERNEL);
	if (!iccom->p) {
		iccom_err("No memory.");
		res = -ENOMEM;
		goto finalize;
	}
	iccom->p->iccom = iccom;

	__iccom_procfs_init(iccom);
	__iccom_statistics_init(iccom);
	__iccom_error_report_init(iccom);

	res = iccom_msg_storage_init(&iccom->p->rx_messages);
	if (res < 0) {
		iccom_err("Could not initialize messages storage.");
		goto free_private;
	}

	res = __iccom_init_packages_storage(iccom);
	if (res < 0) {
		iccom_err("Could not initialize packages storage.");
		goto free_msg_storage;
	}

	// init TX ack/nack data
	iccom->p->ack_val = ICCOM_PACKAGE_ACK_VALUE;
	iccom->p->nack_val = ICCOM_PACKAGE_NACK_VALUE;

	// initial empty package
	res = __iccom_enqueue_empty_tx_data_package(iccom);
	if (res != 0) {
		iccom_err("Could not add initial TX package");
		goto free_pkg_storage;
	}

	__iccom_fillup_next_data_xfer(iccom, &iccom->p->xfer);
	iccom->p->data_xfer_stage = true;

	// init workqueue for delivery to consumer
	res = __iccom_init_workqueue(iccom);
	if (res != 0) {
		iccom_err("Could not init own workqueue.");
		goto free_pkg_storage;
	}

	// initiate consumer notification work
	INIT_WORK(&iccom->p->consumer_delivery_work
		  , __iccom_consumer_notification_routine);

	iccom->p->closing = false;

	// Initializing transport layer and start communication
	res = iccom->xfer_iface.init(iccom->xfer_device
				     , &iccom->p->xfer);

	if (res < 0) {
		iccom_err("Full duplex xfer device failed to"
			  " initialize, err: %d", res);
		goto free_workqueue;
	}

	return 0;

free_workqueue:
	__iccom_close_workqueue(iccom);
free_pkg_storage:
	__iccom_free_packages_storage(iccom);
free_msg_storage:
	iccom_msg_storage_free(&iccom->p->rx_messages);
free_private:
	kfree(iccom->p);
	iccom->p = NULL;
finalize:
	return res;
}

// API
//
// Prints out the statistics message into kernel message buffer
//
// @iccom {valid ptr} device to work with
//
// CONCURRENCE: thread safe
__maybe_unused
void iccom_print_statistics(struct iccom_dev *iccom)
{
	ICCOM_CHECK_DEVICE("no device provided", return);
	ICCOM_CHECK_DEVICE_PRIVATE("broken device data", return);
	ICCOM_CHECK_CLOSING("will not invoke", return);

	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "====== ICCOM (%px) statistics ======", iccom);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "TRANSPORT LAYER: xfers done count:\t%llu"
		       , iccom->p->statistics.transport_layer_xfers_done_count);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "TRANSPORT LAYER: bytes xfered:\t%llu"
		       , iccom->p->statistics.raw_bytes_xfered_via_transport_layer);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "PACKAGES: xfered TOTAL:\t%llu"
		       , iccom->p->statistics.packages_xfered);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "PACKAGES: sent OK:\t%llu"
		       , iccom->p->statistics.packages_sent_ok);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "PACKAGES: received OK:\t%llu"
		       , iccom->p->statistics.packages_received_ok);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "PACKAGES: sent FAIL:\t%llu"
		       , iccom->p->statistics.packages_xfered
			 - iccom->p->statistics.packages_sent_ok);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "PACKAGES: received FAIL:\t%llu"
		       , iccom->p->statistics.packages_xfered
			 - iccom->p->statistics.packages_received_ok);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "PACKAGES: in TX queue:\t%lu"
		       , iccom->p->statistics.packages_in_tx_queue);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "PACKETS: received OK:\t%llu"
		       , iccom->p->statistics.packets_received_ok);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "MESSAGES: received OK:\t%llu"
		       , iccom->p->statistics.messages_received_ok);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "MESSAGES: ready in RX storage:\t%lu"
		       , iccom->p->statistics.messages_ready_in_storage);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL
		       , "BANDWIDTH: total consumer bytes received OK:\t%llu"
		       , iccom->p->statistics.total_consumers_bytes_received_ok);
}

// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
// TODO
// API
//
// Closes the ICCom communication.
//
// NOTE: thread safe
__maybe_unused
void iccom_close(struct iccom_dev *iccom)
{
	ICCOM_CHECK_DEVICE("no device provided", return);
	ICCOM_CHECK_DEVICE_PRIVATE("broken private device part ptr"
				   , return);

	if (IS_ERR_OR_NULL(iccom->xfer_device)) {
	    iccom_err("Looks like provided device doesn't have"
		      " any transport layer device attached");
		return;
	}

	// only one close sequence may run at the same time
	// turning this flag will block all further external
	// calls to given ICCom instance
	bool expected_state = false;
	bool dst_state = true;
	bool res = __atomic_compare_exchange_n(&iccom->p->closing
			, &expected_state, dst_state, false
			, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	if (!res) {
		iccom_err("iccom is already closing now");
		return;
	}
	iccom_info_raw(ICCOM_LOG_INFO_OPT_LEVEL
		       , "closing device (%px)", iccom);

	__iccom_cancel_work_sync(iccom
			, &iccom->p->consumer_delivery_work);

	__iccomm_stop_xfer_device(iccom);

	__iccom_close_workqueue(iccom);

	// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
	// TODO
	// Consider the case when consumer has entered some of our
	// method, but have not yet exited, so we may run into condition
	// when we risk to free the data while some method is still
	// working with it. This will lead to crash.

	// Cleanup all our allocated data
	iccom_msg_storage_free(&iccom->p->rx_messages);
	__iccom_queue_free(iccom);

	__iccom_statistics_close(iccom);
	__iccom_procfs_close(iccom);

	iccom->p->iccom = NULL;
	kfree(iccom->p);
	iccom->p = NULL;
}

// API
//
// Inits underlying full duplex transport and iccom devices in binded.
//
// @iccom {valid ptr to iccom_dev struct} points to unititialized iccom_dev
//      struct
// @full_duplex_if {valid ptr to full_duplex_sym_iface struct} points
//      to valid and filled with correct pointers full_duplex_sym_iface
//      struct
// @full_duplex_device points to the full duplex device structure,
//      which is ready to be used with full_duplex_if->init(...) call.
//
// RETURNS:
//      0: if all fine
//      <0: if failed (negated error code)
__maybe_unused
int iccom_init_binded(struct iccom_dev *iccom
		, const struct full_duplex_sym_iface *const full_duplex_if
		, void *full_duplex_device)
{
	ICCOM_CHECK_DEVICE("no device provided", return -ENODEV);
	ICCOM_CHECK_PTR(full_duplex_device, return -ENODEV);
	if (!__iccom_verify_transport_layer_interface(full_duplex_if)) {
		iccom_err("Not all relevant interface methods are defined");
		return -EINVAL;
	}

	iccom->xfer_device = full_duplex_device;
	iccom->xfer_iface = *full_duplex_if;
	iccom->p = NULL;

	int res = iccom_init(iccom);
	if (res < 0) {
		iccom_err("ICCom driver initialization failed, "
			  "err: %d", res);
		full_duplex_if->close(full_duplex_device);
		return res;
	}

	iccom_info(ICCOM_LOG_INFO_KEY_LEVEL
		   , "iccom & full duplex device inited");
	return 0;
}

// API
//
// Closes both binded full-duplex transport and iccom devices.
//
// @iccom {valid ptr to iccom_dev struct} points to unititialized iccom_dev
//      struct
__maybe_unused
void iccom_close_binded(struct iccom_dev *iccom)
{
	ICCOM_CHECK_DEVICE("no device provided", return);

	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL, "Closing ICCom device");
	iccom_close(iccom);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL, "Closing transport device");
	iccom->xfer_iface.close(iccom->xfer_device);
	iccom_info_raw(ICCOM_LOG_INFO_KEY_LEVEL, "Closing done");
}

// API
//
// Returns true, if the device is running
//
// @iccom {valid ptr to iccom_dev struct} points to unititialized iccom_dev
//      struct
__maybe_unused
bool iccom_is_running(struct iccom_dev *iccom)
{
	return !(IS_ERR_OR_NULL(iccom)
		 || IS_ERR_OR_NULL(iccom->p));
}

/* --------------------- MODULE HOUSEKEEPING SECTION ------------------- */

EXPORT_SYMBOL(iccom_post_message);
EXPORT_SYMBOL(iccom_flush);
EXPORT_SYMBOL(iccom_set_channel_callback);
EXPORT_SYMBOL(iccom_remove_channel_callback);
EXPORT_SYMBOL(iccom_get_channel_callback);
EXPORT_SYMBOL(iccom_read_message);
EXPORT_SYMBOL(iccom_print_statistics);
EXPORT_SYMBOL(iccom_init);
EXPORT_SYMBOL(iccom_close);
EXPORT_SYMBOL(iccom_init_binded);
EXPORT_SYMBOL(iccom_close_binded);
EXPORT_SYMBOL(iccom_is_running);

static int __init iccom_module_init(void)
{
	__iccom_crc32_gen_lookup_table();
	iccom_info(ICCOM_LOG_INFO_KEY_LEVEL, "module loaded");
	return 0;
}

static void __exit iccom_module_exit(void)
{
	iccom_info(ICCOM_LOG_INFO_KEY_LEVEL, "module unloaded");
}

module_init(iccom_module_init);
module_exit(iccom_module_exit);

MODULE_DESCRIPTION("InterChipCommunication protocol module.");
MODULE_AUTHOR("Artem Gulyaev <Artem.Gulyaev@bosch.com>");
MODULE_LICENSE("GPL v2");
