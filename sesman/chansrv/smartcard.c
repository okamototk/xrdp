/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Laxmikant Rashinkar 2013 LK.Rashinkar@gmail.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/*
 * smartcard redirection support
 */

#include <string.h>
#include "os_calls.h"
#include "smartcard.h"
#include "log.h"
#include "irp.h"
#include "devredir.h"
#include "smartcard_pcsc.h"

/*
 * TODO
 *
 * o ensure that all wide calls are handled correctly
 *
 * o need to query client for build number and determine whether we should use
 *   SCREDIR_VERSION_XP or SCREDIR_VERSION_LONGHORN
 *
 * o need to call scard_release_resources()
 *
 * o why is win 7 sending SCARD_IOCTL_ACCESS_STARTED_EVENT first
 * 0000 00 01 00 00 04 00 00 00 e0 00 09 00 00 00 00 00 ................
 * 0010 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
 * 0020 28 b7 9d 02
 */

/*
 * Notes:
 *
 * XP and Server 2003 use version    SCREDIR_VERSION_XP       functions 5 - 58
 * Vista and Server 2008 use version SCREDIR_VERSION_LONGHORN functions 5 - 64
 * if TS Client's build number is >= 4,034 use SCREDIR_VERSION_LONGHORN
 */

/* module based logging */
#define LOG_ERROR   0
#define LOG_INFO    1
#define LOG_DEBUG   2

#ifndef LOG_LEVEL
#define LOG_LEVEL   LOG_INFO
#endif

#define log_error(_params...)                           \
do                                                      \
{                                                       \
    g_write("[%10.10u]: SMART_CARD %s: %d : ERROR: ",   \
            g_time3(), __func__, __LINE__);             \
    g_writeln (_params);                                \
} while (0)

#define log_info(_params...)                            \
do                                                      \
{                                                       \
    if (LOG_INFO <= LOG_LEVEL)                          \
    {                                                   \
        g_write("[%10.10u]: SMART_CARD %s: %d : ",      \
                g_time3(), __func__, __LINE__);         \
        g_writeln (_params);                            \
    }                                                   \
} while (0)

#define log_debug(_params...)                           \
do                                                      \
{                                                       \
    if (LOG_DEBUG <= LOG_LEVEL)                         \
    {                                                   \
        g_write("[%10.10u]: SMART_CARD %s: %d : ",      \
                g_time3(), __func__, __LINE__);         \
        g_writeln (_params);                            \
    }                                                   \
} while (0)

/* [MS-RDPESC] 3.1.4 */
#define SCARD_IOCTL_ESTABLISH_CONTEXT        0x00090014 /* EstablishContext     */
#define SCARD_IOCTL_RELEASE_CONTEXT          0x00090018 /* ReleaseContext       */
#define SCARD_IOCTL_IS_VALID_CONTEXT         0x0009001C /* IsValidContext       */
#define SCARD_IOCTL_LIST_READER_GROUPS       0x00090020 /* ListReaderGroups     */
#define SCARD_IOCTL_LIST_READERS_A           0x00090028 /* ListReaders ASCII    */
#define SCARD_IOCTL_LIST_READERS_W           0x0009002C /* ListReaders Wide     */
#define SCARD_IOCTL_INTRODUCE_READER_GROUP   0x00090050 /* IntroduceReaderGroup */
#define SCARD_IOCTL_FORGET_READER_GROUP      0x00090058 /* ForgetReader         */
#define SCARD_IOCTL_INTRODUCE_READER         0x00090060 /* IntroduceReader      */
#define SCARD_IOCTL_FORGET_READER            0x00090068 /* IntroduceReader      */
#define SCARD_IOCTL_ADD_READER_TO_GROUP      0x00090070 /* AddReaderToGroup     */
#define SCARD_IOCTL_REMOVE_READER_FROM_GROUP 0x00090078 /* RemoveReaderFromGroup*/
#define SCARD_IOCTL_GET_STATUS_CHANGE_A      0x000900A0 /* GetStatusChangeA     */
#define SCARD_IOCTL_GET_STATUS_CHANGE_W      0x000900A4 /* GetStatusChangeW     */
#define SCARD_IOCTL_CANCEL                   0x000900A8 /* Cancel               */
#define SCARD_IOCTL_CONNECT_A                0x000900AC /* ConnectA             */
#define SCARD_IOCTL_CONNECT_W                0x000900B0 /* ConnectW             */
#define SCARD_IOCTL_RECONNECT                0x000900B4 /* Reconnect            */
#define SCARD_IOCTL_DISCONNECT               0x000900B8 /* Disconnect           */
#define SCARD_IOCTL_BEGIN_TRANSACTION        0x000900BC /* BeginTransaction     */
#define SCARD_IOCTL_END_TRANSACTION          0x000900C0 /* EndTransaction       */
#define SCARD_IOCTL_STATE                    0x000900C4 /* State                */
#define SCARD_IOCTL_STATUS_A                 0x000900C8 /* StatusA              */
#define SCARD_IOCTL_STATUS_W                 0x000900CC /* StatusW              */
#define SCARD_IOCTL_TRANSMIT                 0x000900D0 /* Transmit             */
#define SCARD_IOCTL_CONTROL                  0x000900D4 /* Control              */
#define SCARD_IOCTL_GETATTRIB                0x000900D8 /* GetAttrib            */
#define SCARD_IOCTL_SETATTRIB                0x000900DC /* SetAttrib            */
#define SCARD_IOCTL_ACCESS_STARTED_EVENT     0x000900E0 /* SCardAccessStartedEvent */
#define SCARD_IOCTL_LOCATE_CARDS_BY_ATR      0x000900E8 /* LocateCardsByATR     */

/* scope used in EstablishContextCall */
#define SCARD_SCOPE_USER                     0x00000000
#define SCARD_SCOPE_TERMINAL                 0x00000001
#define SCARD_SCOPE_SYSTEM                   0x00000002

/* disposition - action to take on card */
#define SCARD_LEAVE_CARD                     0x00000000
#define SCARD_RESET_CARD                     0x00000001
#define SCARD_UNPOWER_CARD                   0x00000002
#define SCARD_EJECT_CARD                     0x00000003

#define MAX_SMARTCARDS                       16

/* stores info about a smart card */
typedef struct smartcard
{
    tui32 DeviceId;
} SMARTCARD;

/* globals */
SMARTCARD*   smartcards[MAX_SMARTCARDS];
int          g_smartcards_inited = 0;
static tui32 g_device_id = 0;
static int   g_scard_index = 0;

/* externs */
extern tui32 g_completion_id;
extern int   g_rdpdr_chan_id;    /* in chansrv.c */


/******************************************************************************
**                   static functions local to this file                     **
******************************************************************************/
static struct stream * APP_CC scard_make_new_ioctl(IRP *irp, tui32 ioctl);
static int  APP_CC scard_add_new_device(tui32 device_id);
static int  APP_CC scard_get_free_slot(void);
#if 0
static void APP_CC scard_release_resources(void);
#endif
static void APP_CC scard_send_EstablishContext(IRP* irp, int scope);
static void APP_CC scard_send_ReleaseContext(IRP* irp, tui32 context);
static void APP_CC scard_send_IsContextValid(IRP* irp, tui32 context);
static void APP_CC scard_send_ListReaders(IRP* irp, tui32 context, int wide);
static void APP_CC scard_send_GetStatusChange(IRP* irp, tui32 context, int wide,
                                              tui32 timeout, tui32 num_readers,
                                              READER_STATE* rsa);
static void APP_CC scard_send_Connect(IRP* irp, tui32 context, int wide,
                                      READER_STATE* rs);
static void APP_CC scard_send_Reconnect(IRP* irp, tui32 context,
                                        tui32 sc_handle, READER_STATE* rs);
static void APP_CC scard_send_BeginTransaction(IRP* irp, tui32 sc_handle);
static void APP_CC scard_send_EndTransaction(IRP* irp, tui32 sc_handle,
                                             tui32 dwDisposition);
static void APP_CC scard_send_Status(IRP* irp, int wide, tui32 sc_handle,
                                     int cchReaderLen, int cbAtrLen);
static void APP_CC scard_send_Disconnect(IRP* irp, tui32 context,
                                         tui32 sc_handle, int dwDisposition);
static int  APP_CC scard_send_Transmit(IRP* irp, tui32 sc_handle,
                                       char *send_data, int send_bytes,
                                       int recv_bytes,
                                       struct xrdp_scard_io_request *send_ior,
                                       struct xrdp_scard_io_request *recv_ior);
static int APP_CC scard_send_Control(IRP* irp, tui32 context, tui32 sc_handle,
                                     char *send_data, int send_bytes,
                                     int recv_bytes, int control_code);
static int APP_CC scard_send_Cancel(IRP* irp, tui32 context);
static int APP_CC scard_send_GetAttrib(IRP* irp, tui32 sc_handle,
                                       READER_STATE* rs);

/******************************************************************************
**                    local callbacks into this module                       **
******************************************************************************/

static void APP_CC scard_handle_EstablishContext_Return(struct stream *s, IRP *irp,
                                                 tui32 DeviceId, tui32 CompletionId,
                                                 tui32 IoStatus);

static void APP_CC scard_handle_ReleaseContext_Return(struct stream *s, IRP *irp,
                                               tui32 DeviceId, tui32 CompletionId,
                                               tui32 IoStatus);


static void APP_CC scard_handle_IsContextValid_Return(struct stream *s, IRP *irp,
                                            tui32 DeviceId, tui32 CompletionId,
                                            tui32 IoStatus);

static void APP_CC scard_handle_ListReaders_Return(struct stream *s, IRP *irp,
                                            tui32 DeviceId, tui32 CompletionId,
                                            tui32 IoStatus);

static void APP_CC scard_handle_GetStatusChange_Return(struct stream *s, IRP *irp,
                                                tui32 DeviceId, tui32 CompletionId,
                                                tui32 IoStatus);

static void APP_CC scard_handle_Connect_Return(struct stream *s, IRP *irp,
                                        tui32 DeviceId, tui32 CompletionId,
                                        tui32 IoStatus);

static void APP_CC scard_handle_Reconnect_Return(struct stream *s, IRP *irp,
                                                 tui32 DeviceId, tui32 CompletionId,
                                                 tui32 IoStatus);

static void APP_CC scard_handle_BeginTransaction_Return(struct stream *s, IRP *irp,
                                                 tui32 DeviceId, tui32 CompletionId,
                                                 tui32 IoStatus);

static void APP_CC scard_handle_EndTransaction_Return(struct stream *s, IRP *irp,
                                               tui32 DeviceId,
                                               tui32 CompletionId,
                                               tui32 IoStatus);

static void APP_CC scard_handle_Status_Return(struct stream *s, IRP *irp,
                                       tui32 DeviceId, tui32 CompletionId,
                                       tui32 IoStatus);

static void APP_CC scard_handle_Disconnect_Return(struct stream *s, IRP *irp,
                                           tui32 DeviceId, tui32 CompletionId,
                                           tui32 IoStatus);


static void APP_CC scard_handle_Transmit_Return(struct stream *s, IRP *irp,
                                                tui32 DeviceId,
                                                tui32 CompletionId,
                                                tui32 IoStatus);

static void APP_CC scard_handle_Control_Return(struct stream *s, IRP *irp,
                                                tui32 DeviceId,
                                                tui32 CompletionId,
                                                tui32 IoStatus);

static void APP_CC scard_handle_Cancel_Return(struct stream *s, IRP *irp,
                                              tui32 DeviceId,
                                              tui32 CompletionId,
                                              tui32 IoStatus);

static void APP_CC scard_handle_GetAttrib_Return(struct stream *s, IRP *irp,
                                                 tui32 DeviceId,
                                                 tui32 CompletionId,
                                                 tui32 IoStatus);

/******************************************************************************
**                                                                           **
**          externally accessible functions, defined in smartcard.h          **
**                                                                           **
******************************************************************************/

/**
 *****************************************************************************/
void APP_CC
scard_device_announce(tui32 device_id)
{
    log_debug("entered: device_id=%d", device_id);

    if (g_smartcards_inited)
        return;

    g_memset(&smartcards, 0, sizeof(smartcards));
    g_smartcards_inited = 1;
    g_device_id = device_id;
    g_scard_index = scard_add_new_device(device_id);

    if (g_scard_index < 0)
        log_debug("scard_add_new_device failed with DeviceId=%d", g_device_id);
    else
        log_debug("added smartcard with DeviceId=%d to list", g_device_id);
}

/**
 *
 *****************************************************************************/
int APP_CC
scard_get_wait_objs(tbus *objs, int *count, int *timeout)
{
    return scard_pcsc_get_wait_objs(objs, count, timeout);
}

/**
 *
 *****************************************************************************/
int APP_CC
scard_check_wait_objs(void)
{
    return scard_pcsc_check_wait_objs();
}

/**
 *
 *****************************************************************************/
int APP_CC
scard_init(void)
{
    log_debug("init");
    return scard_pcsc_init();
}

/**
 *
 *****************************************************************************/
int APP_CC
scard_deinit(void)
{
    log_debug("deinit");
    return scard_pcsc_deinit();
}

/**
 *
 *****************************************************************************/
int APP_CC
scard_send_establish_context(struct trans *con, int scope)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_EstablishContext_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_EstablishContext(irp, scope);

    return 0;
}

/**
 * Release a previously established Smart Card context
 *****************************************************************************/
int APP_CC
scard_send_release_context(struct trans *con, tui32 context)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_ReleaseContext_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_ReleaseContext(irp, context);

    return 0;
}

/**
 * Checks if a previously established context is still valid
 *****************************************************************************/
int APP_CC
scard_send_is_valid_context(struct trans *con, tui32 context)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_IsContextValid_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_IsContextValid(irp, context);

    return 0;
}

/**
 *
 *****************************************************************************/
int APP_CC
scard_send_list_readers(struct trans *con, tui32 context, int wide)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }
    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_ListReaders_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_ListReaders(irp, context, wide);

    return 0;
}

/**
 * Send get change in status command
 *
 * @param  con          connection to client
 * @param  wide         TRUE if unicode string
 * @param  timeout      timeout in milliseconds, -1 for infinity
 * @param  num_readers  number of entries in rsa
 * @param  rsa          array of READER_STATEs
 *****************************************************************************/
int APP_CC
scard_send_get_status_change(struct trans *con, tui32 context, int wide,
                             tui32 timeout, tui32 num_readers,
                             READER_STATE* rsa)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_GetStatusChange_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_GetStatusChange(irp, context, wide, timeout, num_readers, rsa);

    return 0;
}

/**
 * Open a connection to the smart card located in the reader
 *
 * @param  con   connection to client
 * @param  wide  TRUE if unicode string
 *****************************************************************************/
int APP_CC
scard_send_connect(struct trans *con, tui32 context, int wide,
                   READER_STATE* rs)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_Connect_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_Connect(irp, context, wide, rs);

    return 0;
}

/**
 * The reconnect method re-establishes a smart card reader handle. On success,
 * the handle is valid once again.
 *
 * @param  con        connection to client
 * @param  sc_handle  handle to device
 * @param  rs         reader state where following fields are set
 *                        rs.shared_mode_flag
 *                        rs.preferred_protocol
 *                        rs.init_type
 *****************************************************************************/
int APP_CC
scard_send_reconnect(struct trans *con, tui32 context, tui32 sc_handle,
                     READER_STATE* rs)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_Reconnect_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_Reconnect(irp, context, sc_handle, rs);

    return 0;
}

/**
 * Lock smart card reader for exclusive access for specified smart
 * card reader handle.
 *
 * @param  con   connection to client
 *****************************************************************************/
int APP_CC
scard_send_begin_transaction(struct trans *con, tui32 sc_handle)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_BeginTransaction_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_BeginTransaction(irp, sc_handle);

    return 0;
}

/**
 * Release a smart card reader after being locked by a previously
 * successful call to Begin Transaction
 *
 * @param  con        connection to client
 * @param  sc_handle  handle to smartcard
 *****************************************************************************/
int APP_CC
scard_send_end_transaction(struct trans *con, tui32 sc_handle,
                           tui32 dwDisposition)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_EndTransaction_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_EndTransaction(irp, sc_handle, dwDisposition);

    return 0;
}

/**
 * Get the status of a connection for a valid smart card reader handle
 *
 * @param  con   connection to client
 * @param  wide  TRUE if unicode string
 *****************************************************************************/
int APP_CC
scard_send_status(struct trans *con, int wide, tui32 sc_handle,
                  int cchReaderLen, int cbAtrLen)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_Status_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_Status(irp, wide, sc_handle, cchReaderLen, cbAtrLen);

    return 0;
}

/**
 * Release a smart card reader handle that was acquired in ConnectA/ConnectW
 *
 * @param  con        connection to client
 * @param  sc_handle  handle to smartcard
 *****************************************************************************/
int APP_CC
scard_send_disconnect(struct trans *con, tui32 context, tui32 sc_handle,
                      int dwDisposition)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_Disconnect_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_Disconnect(irp, context, sc_handle, dwDisposition);

    return 0;
}

/**
 * The Transmit_Call structure is used to send data to the smart card
 * associated with a valid context.
 *****************************************************************************/
int APP_CC
scard_send_transmit(struct trans *con, tui32 sc_handle,
                    char *send_data, int send_bytes, int recv_bytes,
                    struct xrdp_scard_io_request *send_ior,
                    struct xrdp_scard_io_request *recv_ior)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_Transmit_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_Transmit(irp, sc_handle, send_data, send_bytes, recv_bytes,
                        send_ior, recv_ior);

    return 0;
}

/**
 * Communicate directly with the smart card reader
 *****************************************************************************/
int APP_CC
scard_send_control(struct trans *con, tui32 context, tui32 sc_handle,
                   char *send_data, int send_bytes,
                   int recv_bytes, int control_code)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_Control_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_Control(irp, context, sc_handle, send_data,
                       send_bytes, recv_bytes, control_code);

    return 0;
}

/**
 * Cancel any outstanding calls
 *****************************************************************************/
int APP_CC
scard_send_cancel(struct trans *con, tui32 context)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_Cancel_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_Cancel(irp, context);

    return 0;
}

/**
 * Get reader attributes
 *****************************************************************************/
int APP_CC
scard_send_get_attrib(struct trans *con, tui32 sc_handle, READER_STATE* rs)
{
    IRP *irp;

    /* setup up IRP */
    if ((irp = devredir_irp_new()) == NULL)
    {
        log_error("system out of memory");
        return 1;
    }

    irp->scard_index = g_scard_index;
    irp->CompletionId = g_completion_id++;
    irp->DeviceId = g_device_id;
    irp->callback = scard_handle_GetAttrib_Return;
    irp->user_data = con;

    /* send IRP to client */
    scard_send_GetAttrib(irp, sc_handle, rs);

    return 0;
}

/******************************************************************************
**                                                                           **
**                   static functions local to this file                     **
**                                                                           **
******************************************************************************/

/**
 * Crate a new stream and insert specified IOCTL
 *
 * @param  irp    information about the I/O
 * @param  ioctl  the IOCTL code
 *
 * @return stream with IOCTL inserted in it, NULL on error
 *****************************************************************************/
static struct stream * APP_CC
scard_make_new_ioctl(IRP *irp, tui32 ioctl)
{
    /*
     * format of device control request
     *
     * DeviceIoRequest
     * u16       RDPDR_CTYP_CORE
     * u16       PAKID_CORE_DEVICE_IOREQUEST
     * u32       DeviceId
     * u32       FileId
     * u32       CompletionId
     * u32       MajorFunction
     * u32       MinorFunction
     *
     * u32       OutputBufferLength SHOULD be 2048
     * u32       InputBufferLength
     * u32       IoControlCode
     * 20 bytes  padding
     * xx bytes  InputBuffer (variable)
     */

    struct stream *s;

    xstream_new(s, 1024 * 4);
    if (s == NULL)
    {
        log_error("system out of memory");
        return s;
    }

    devredir_insert_DeviceIoRequest(s,
                                    irp->DeviceId,
                                    irp->FileId,
                                    irp->CompletionId,
                                    IRP_MJ_DEVICE_CONTROL,
                                    0);

    xstream_wr_u32_le(s, 2048);        /* OutputBufferLength               */
    xstream_wr_u32_le(s, 0);           /* InputBufferLength - insert later */
    xstream_wr_u32_le(s, ioctl);       /* Ioctl Code                       */
    xstream_seek(s, 20);               /* padding                          */

    /* [MS-RPCE] 2.2.6.1 */
    xstream_wr_u32_le(s, 0x00081001);  /* len 8, LE, v1                    */
    xstream_wr_u32_le(s, 0xcccccccc);  /* filler                           */

    return s;
}

/**
 * Create a new smart card device entry and insert it into smartcards[]
 *
 * @param  device_id  DeviceId of new card
 *
 * @return index into smartcards[] on success, -1 on failure
 *****************************************************************************/
static int APP_CC
scard_add_new_device(tui32 device_id)
{
    int        index;
    SMARTCARD *sc;

    if ((index = scard_get_free_slot()) < 0)
    {
        log_error("scard_get_free_slot failed");
        return -1;
    }

    if ((sc = g_malloc(sizeof(SMARTCARD), 1)) == NULL)
    {
        log_error("system out of memory");
        return -1;
    }

    sc->DeviceId = device_id;
    smartcards[index] = sc;

    return index;
}

/**
 * Find first unused entry in smartcards
 *
 * @return index of first unused entry in smartcards or -1 if smartcards
 * is full
 *****************************************************************************/
static int APP_CC
scard_get_free_slot(void)
{
    int i;

    for (i = 0; i < MAX_SMARTCARDS; i++)
    {
        if (smartcards[i] == NULL)
        {
            log_debug("found free slot at index %d", i);
            return i;
        }
    }

    log_error("too many smart card devices; rejecting this one");
    return -1;
}

#if 0
/**
 * Release resources prior to shutting down
 *****************************************************************************/
static void APP_CC
scard_release_resources(void)
{
    int i;

    for (i = 0; i < MAX_SMARTCARDS; i++)
    {
        if (smartcards[i] != NULL)
        {
            g_free(smartcards[i]);
            smartcards[i] = NULL;
        }
    }
}
#endif

/**
 *
 *****************************************************************************/
static void APP_CC
scard_send_EstablishContext(IRP *irp, int scope)
{
    struct stream *s;
    int            bytes;

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_ESTABLISH_CONTEXT)) == NULL)
    {
        log_error("scard_make_new_ioctl failed");
        return;
    }

    xstream_wr_u32_le(s, 0x08);   /* len                      */
    xstream_wr_u32_le(s, 0);      /* unused                   */
    xstream_wr_u32_le(s, scope);  /* Ioctl specific data      */
    xstream_wr_u32_le(s, 0);      /* don't know what this is, */
                                  /* but Win7 is sending it   */
    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * Release a previously established Smart Card context
 *****************************************************************************/
static void APP_CC
scard_send_ReleaseContext(IRP* irp, tui32 context)
{
    /* see [MS-RDPESC] 3.1.4.2 */

    SMARTCARD*     sc;
    struct stream* s;
    int            bytes;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_RELEASE_CONTEXT)) == NULL)
        return;

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *        4 bytes    len - don't know what this is, zero for now
     *       12 bytes    unused
     * u32    4 bytes    context len
     *        4 bytes    context
     */

    xstream_wr_u32_le(s, 0);
    xstream_seek(s, 12);

    /* insert context */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * Checks if a previously established context is still valid
 *****************************************************************************/
static void APP_CC
scard_send_IsContextValid(IRP* irp, tui32 context)
{
    /* see [MS-RDPESC] 3.1.4.3 */

    SMARTCARD*     sc;
    struct stream* s;
    int            bytes;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_IS_VALID_CONTEXT)) == NULL)
        return;

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       16 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    context len
     * u32    4 bytes    context
     */

    xstream_wr_u32_le(s, 16);

    /* insert context */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 *
 *****************************************************************************/
static void APP_CC
scard_send_ListReaders(IRP *irp, tui32 context, int wide)
{
    /* see [MS-RDPESC] 2.2.2.4 */

    SMARTCARD     *sc;
    struct stream *s;
    int            bytes;
    tui32          ioctl;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    ioctl = (wide) ? SCARD_IOCTL_LIST_READERS_W :
                     SCARD_IOCTL_LIST_READERS_A;

    if ((s = scard_make_new_ioctl(irp, ioctl)) == NULL)
        return;

    xstream_wr_u32_le(s, 72); /* number of bytes to follow */
    xstream_seek(s, 28);      /* freerdp does not use this */

    /* insert context */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);

    xstream_wr_u32_le(s, 36); /* length of mszGroups */
    xstream_wr_u16_le(s, 0x0053);
    xstream_wr_u16_le(s, 0x0043);
    xstream_wr_u16_le(s, 0x0061);
    xstream_wr_u16_le(s, 0x0072);
    xstream_wr_u16_le(s, 0x0064);
    xstream_wr_u16_le(s, 0x0024);
    xstream_wr_u16_le(s, 0x0041);
    xstream_wr_u16_le(s, 0x006c);
    xstream_wr_u16_le(s, 0x006c);
    xstream_wr_u16_le(s, 0x0052);
    xstream_wr_u16_le(s, 0x0065);
    xstream_wr_u16_le(s, 0x0061);
    xstream_wr_u16_le(s, 0x0064);
    xstream_wr_u16_le(s, 0x0065);
    xstream_wr_u16_le(s, 0x0072);
    xstream_wr_u16_le(s, 0x0073);

    xstream_wr_u32_le(s, 0x00);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding  */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);

    /*
    scard_device_control: dumping 120 bytes of data
    0000 00 08 00 00 58 00 00 00 2c 00 09 00 00 00 00 00 ....X...,.......
    0010 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
    0020 01 10 08 00 cc cc cc cc 48 00 00 00 00 00 00 00 ........H.......
    0030 04 00 00 00 00 00 02 00 24 00 00 00 04 00 02 00 ........$.......
    0040 00 00 00 00 ff ff ff ff 04 00 00 00 84 db 03 01 ................
    0050 24 00 00 00 53 00 43 00 61 00 72 00 64 00 24 00 $...S.C.a.r.d.$.
    0060 41 00 6c 00 6c 00 52 00 65 00 61 00 64 00 65 00 A.l.l.R.e.a.d.e.
    0070 72 00 73 00 00 00 00 00                         r.s.....
    scard_device_control: output_len=2048 input_len=88 ioctl_code=0x9002c
    */

    /*
    scard_device_control: dumping 120 bytes of data
    0000 00 08 00 00 80 00 00 00 14 00 09 00 00 00 00 00 ................
    0010 2e 2e 00 00 00 00 00 00 02 00 00 00 00 00 00 00 ................
    0020 01 10 08 00 cc cc cc cc 48 00 00 00 00 00 00 00 ........H.......
    0030 02 00 00 00 00 00 00 00 72 64 00 00 00 00 00 00 ........rd......
    0040 81 27 00 00 00 00 00 00 04 00 00 00 84 b3 03 01 .'..............
    0050 24 00 00 00 53 00 43 00 61 00 72 00 64 00 24 00 $...S.C.a.r.d.$.
    0060 41 00 6c 00 6c 00 52 00 65 00 61 00 64 00 65 00 A.l.l.R.e.a.d.e.
    0070 72 00 73 00 00 00 00 00                         r.s.....
    scard_device_control: output_len=2048 input_len=128 ioctl_code=0x90014
    */
}

/*****************************************************************************/
static int
align_s(struct stream *s, int bytes)
{
    int i32;

    i32 = (int) (s->p - s->data);
    while ((i32 % bytes) != 0)
    {
        out_uint8s(s, 1);
        i32 = (int) (s->p - s->data);
    }
    return 0;
}

/**
 * Get change in status
 *
 * @param  irp          I/O resource pkt
 * @param  wide         TRUE if unicode string
 * @param  timeout      timeout in milliseconds, -1 for infinity
 * @param  num_readers  number of entries in rsa
 * @param  rsa          array of READER_STATEs
 *****************************************************************************/
static void APP_CC
scard_send_GetStatusChange(IRP* irp, tui32 context, int wide, tui32 timeout,
                           tui32 num_readers, READER_STATE* rsa)
{
    /* see [MS-RDPESC] 2.2.2.11 for ASCII     */
    /* see [MS-RDPESC] 2.2.2.12 for Wide char */

    SMARTCARD*     sc;
    READER_STATE*  rs;
    struct stream* s;
    tui32          ioctl;
    int            bytes;
    int            i;
    int            num_chars;
    int            index;
    twchar         w_reader_name[100];

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    ioctl = (wide) ? SCARD_IOCTL_GET_STATUS_CHANGE_W :
                     SCARD_IOCTL_GET_STATUS_CHANGE_A;

    if ((s = scard_make_new_ioctl(irp, ioctl)) == NULL)
        return;

    xstream_seek(s, 16);                /* unused */
    xstream_wr_u32_le(s, timeout);
    xstream_wr_u32_le(s, num_readers);
    xstream_wr_u32_le(s, 0);            /* unused */

    /* insert context */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);

    xstream_wr_u32_le(s, 0);            /* unused */

    /* insert card reader state */
    for (i = 0; i < num_readers; i++)
    {
        rs = &rsa[i];
        xstream_wr_u32_le(s, 0);        /* unused */
        xstream_wr_u32_le(s, rs->current_state);
        xstream_wr_u32_le(s, rs->event_state);
        xstream_wr_u32_le(s, rs->atr_len);
        xstream_copyin(s, rs->atr, 33);
        out_uint8s(s, 3);
    }

    if (wide)
    {
        /* insert card reader names */
        for (i = 0; i < num_readers; i++)
        {
            rs = &rsa[i];
            num_chars = g_mbstowcs(w_reader_name, rs->reader_name, 99);
            xstream_wr_u32_le(s, 0);        /* unused */
            xstream_wr_u32_le(s, 0);        /* unused */
            xstream_wr_u32_le(s, num_chars);
            for (index = 0; index < num_chars; index++)
            {
                xstream_wr_u16_le(s, w_reader_name[index]);
            }
            align_s(s, 4);
        }
    }
    else
    {
        /* insert card reader names */
        for (i = 0; i < num_readers; i++)
        {
            rs = &rsa[i];
            num_chars = g_mbstowcs(w_reader_name, rs->reader_name, 99);
            xstream_wr_u32_le(s, 0);        /* unused */
            xstream_wr_u32_le(s, 0);        /* unused */
            xstream_wr_u32_le(s, num_chars);
            for (index = 0; index < num_chars; index++)
            {
                xstream_wr_u8(s, w_reader_name[index]);
            }
            align_s(s, 4);
        }
    }

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * Send connect command
 *
 * @param  irp  I/O resource pkt
 * @param  wide TRUE if unicode string
 * @param  rs   reader state
 *****************************************************************************/
static void APP_CC
scard_send_Connect(IRP* irp, tui32 context, int wide, READER_STATE* rs)
{
    /* see [MS-RDPESC] 2.2.2.13 for ASCII     */
    /* see [MS-RDPESC] 2.2.2.14 for Wide char */

    SMARTCARD*     sc;
    struct stream* s;
    tui32          ioctl;
    int            bytes;
    int            num_chars;
    int            index;
    twchar         w_reader_name[100];

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    ioctl = (wide) ? SCARD_IOCTL_CONNECT_W :
                     SCARD_IOCTL_CONNECT_A;

    if ((s = scard_make_new_ioctl(irp, ioctl)) == NULL)
        return;

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       20 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    dwShareMode
     * u32    4 bytes    dwPreferredProtocols
     *       xx bytes    reader name
     * u32    4 bytes    context length (len)
     *      len bytes    context
     */

    xstream_seek(s, 20);
    xstream_wr_u32_le(s, rs->dwShareMode);
    xstream_wr_u32_le(s, rs->dwPreferredProtocols);

    /* insert reader name */
    num_chars = g_mbstowcs(w_reader_name, rs->reader_name, 99);
    xstream_wr_u32_le(s, 0);
    xstream_wr_u32_le(s, 0);
    xstream_wr_u32_le(s, num_chars);
    if (wide)
    {
        for (index = 0; index < num_chars; index++)
        {
            xstream_wr_u16_le(s, w_reader_name[index]);
        }
    }
    else
    {
        for (index = 0; index < num_chars; index++)
        {
            xstream_wr_u8(s, w_reader_name[index]);
        }
    }
    align_s(s, 4);

    /* insert context */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * The reconnect method re-establishes a smart card reader handle. On success,
 * the handle is valid once again.
 *
 * @param  con        connection to client
 * @param  sc_handle  handle to device
 * @param  rs         reader state where following fields are set
 *                        rs.shared_mode_flag
 *                        rs.preferred_protocol
 *                        rs.init_type
 *****************************************************************************/
static void APP_CC
scard_send_Reconnect(IRP* irp, tui32 context, tui32 sc_handle, READER_STATE* rs)
{
    /* see [MS-RDPESC] 2.2.2.15 */
    /* see [MS-RDPESC] 3.1.4.36 */

    SMARTCARD*     sc;
    struct stream* s;
    int            bytes;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_RECONNECT)) == NULL)
        return;

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       24 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    dwShareMode
     * u32    4 bytes    dwPreferredProtocols
     * u32    4 bytes    dwInitialization
     * u32    4 bytes    context length
     * u32    4 bytes    context
     * u32    4 bytes    handle length
     * u32    4 bytes    handle
     */

    xstream_seek(s, 24);
    xstream_wr_u32_le(s, rs->dwShareMode);
    xstream_wr_u32_le(s, rs->dwPreferredProtocols);
    xstream_wr_u32_le(s, rs->init_type);
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, sc_handle);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * Lock smart card reader for exclusive access for specified smart
 * card reader handle.
 *
 * @param  con   connection to client
 *****************************************************************************/
static void APP_CC
scard_send_BeginTransaction(IRP *irp, tui32 sc_handle)
{
    /* see [MS-RDPESC] 4.9 */

    SMARTCARD     *sc;
    struct stream *s;
    int            bytes;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_BEGIN_TRANSACTION)) == NULL)
        return;

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       36 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    len of sc_handle
     *        4 bytes    sc_handle
     */

    xstream_seek(s, 36);

    /* insert handle */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, sc_handle);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * Release a smart card reader after being locked by a previously
 * successful call to Begin Transaction
 *
 * @param  con        connection to client
 * @param  sc_handle  handle to smartcard
 *****************************************************************************/
static void APP_CC
scard_send_EndTransaction(IRP *irp, tui32 sc_handle, tui32 dwDisposition)
{
    /* see [MS-RDPESC] 3.1.4.32 */

    SMARTCARD     *sc;
    struct stream *s;
    int            bytes;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_END_TRANSACTION)) == NULL)
        return;

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       24 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    disposition
     *        8 unused
     * u32    4 bytes    length of sc_handle
     *        4 bytes    sc_handle
     */

    xstream_seek(s, 24);
    xstream_wr_u32_le(s, dwDisposition);
    xstream_seek(s, 8);

    /* insert handle */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, sc_handle);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * Get the status of a connection for a valid smart card reader handle
 *
 * @param  con   connection to client
 * @param  wide  TRUE if unicode string
 *****************************************************************************/
static void APP_CC
scard_send_Status(IRP *irp, int wide, tui32 sc_handle,
                  int cchReaderLen, int cbAtrLen)
{
    /* see [MS-RDPESC] 2.2.2.18 */

    SMARTCARD     *sc;
    struct stream *s;
    int            bytes;
    tui32          ioctl;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    ioctl = wide ? SCARD_IOCTL_STATUS_W : SCARD_IOCTL_STATUS_A;
    if ((s = scard_make_new_ioctl(irp, ioctl)) == NULL)
    {
        log_error("scard_make_new_ioctl");
        return;
    }

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       28 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    reader len
     * u32    4 bytes    ATR len
     *        8 bytes    unused
     * u32    4 bytes    len of sc_handle
     *        4 bytes    sc_handle
     *        4 bytes    unused
     */

    xstream_seek(s, 28);
    xstream_wr_u32_le(s, cchReaderLen); /* readerLen, see [MS-RDPESC] 4.11 */
    xstream_wr_u32_le(s, cbAtrLen); /* atrLen,    see [MS-RDPESC] 4.11 */
    xstream_seek(s, 8);

    /* insert sc_handle */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, sc_handle);

    xstream_wr_u32_le(s, 0);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * Release a smart card reader handle that was acquired in ConnectA/ConnectW
 *
 * @param  con        connection to client
 * @param  sc_handle  handle to smartcard
 *****************************************************************************/
static void APP_CC
scard_send_Disconnect(IRP *irp, tui32 context, tui32 sc_handle,
                      int dwDisposition)
{
    /* see [MS-RDPESC] 3.1.4.30 */

    SMARTCARD*     sc;
    struct stream* s;
    int            bytes;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_DISCONNECT)) == NULL)
        return;

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       24 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    disposition
     * u32    4 bytes    context len
     *        4 bytes    context
     * u32    4 bytes    length of sc_handle
     *        4 bytes    sc_handle
     */

    xstream_seek(s, 24);
    xstream_wr_u32_le(s, dwDisposition);

    /* insert context */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);

    /* insert handle */
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, sc_handle);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
}

/**
 * The Transmit_Call structure is used to send data to the smart card
 * associated with a valid context.
 *****************************************************************************/
static int APP_CC
scard_send_Transmit(IRP* irp, tui32 sc_handle, char *send_data,
                    int send_bytes, int recv_bytes,
                    struct xrdp_scard_io_request *send_ior,
                    struct xrdp_scard_io_request *recv_ior)
{
    /* see [MS-RDPESC] 2.2.2.19 */

    SMARTCARD*     sc;
    struct stream* s;
    int            bytes;
    int            val;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return 1;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_TRANSMIT)) == NULL)
    {
        log_error("scard_make_new_ioctl");
        return 1;
    }

    log_debug("send_bytes %d recv_bytes %d send dwProtocol %d cbPciLength %d "
              "extra_bytes %d recv dwProtocol %d cbPciLength %d", send_bytes,
              recv_bytes, send_ior->dwProtocol, send_ior->cbPciLength,
              send_ior->extra_bytes, recv_ior->dwProtocol, recv_ior->cbPciLength,
              recv_ior->extra_bytes);

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       12 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    map0
     *        4 bytes    unused
     * u32    4 bytes    map1
     * u32    4 bytes    dwProtocol
     * u32    4 bytes    cbPciLength
     * u32    4 bytes    map2
     * u32    4 byts     cbSendLength
     * u32    4 bytes    map3
     * u32    4 bytes    map4
     * u32    4 bytes    map5
     * u32    4 bytes    map6
     * u32    4 bytes    cbRecvLength
     * u32    4 bytes    len of sc_handle
     * u32    4 bytes    sc_handle
     */

    xstream_seek(s, 12);
    xstream_wr_u32_le(s, 0); // map0
    xstream_seek(s, 4);
    xstream_wr_u32_le(s, 0); // map1
    xstream_wr_u32_le(s, send_ior->dwProtocol);
    xstream_wr_u32_le(s, send_ior->cbPciLength);
    val = send_ior->extra_bytes > 0 ? 1 : 0;
    xstream_wr_u32_le(s, val); // map2
    xstream_wr_u32_le(s, send_bytes);
    val = send_bytes > 0 ? 1 : 0;
    xstream_wr_u32_le(s, val); // map3
    val = recv_ior->cbPciLength > 0 ? 1 : 0;
    xstream_wr_u32_le(s, val); // map 4
    xstream_wr_u32_le(s, 0); // map5
    xstream_wr_u32_le(s, recv_bytes);
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, sc_handle);

    if (send_ior->extra_bytes > 0)
    {
        xstream_wr_u32_le(s, send_ior->extra_bytes);
        out_uint8a(s, send_ior->extra_data, send_ior->extra_bytes);
    }

    if (send_bytes > 0)
    {
        xstream_wr_u32_le(s, send_bytes);
        out_uint8a(s, send_data, send_bytes);
    }

    if (recv_ior->cbPciLength > 0)
    {
        xstream_wr_u32_le(s, recv_ior->dwProtocol);
        xstream_wr_u32_le(s, recv_ior->cbPciLength);
        val = recv_ior->extra_bytes > 0 ? 1 : 0;
        xstream_wr_u32_le(s, val);
        if (val)
        {
            xstream_wr_u32_le(s, recv_ior->extra_bytes);
            out_uint8a(s, recv_ior->extra_data, recv_ior->extra_bytes);
        }
    }

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
    return 0;
}

/**
 * Communicate directly with the smart card reader
 *****************************************************************************/
static int APP_CC
scard_send_Control(IRP* irp, tui32 context, tui32 sc_handle, char *send_data,
                   int send_bytes, int recv_bytes, int control_code)
{
    /* see [MS-RDPESC] 2.2.2.19 */

    SMARTCARD*     sc;
    struct stream* s;
    int            bytes;
    int            val;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return 1;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_CONTROL)) == NULL)
    {
        log_error("scard_make_new_ioctl");
        return 1;
    }

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       12 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    map0
     *        4 bytes    unused
     * u32    4 bytes    map1
     * u32    4 bytes    dwControlCode
     * u32    4 bytes    cbRecvLength
     * u32    4 bytes    map2
     *        4 bytes    unused
     * u32    4 bytes    cbOutBufferSize
     * u32    4 bytes    context len
     * u32    4 bytes    context
     * u32    4 bytes    handle len
     * u32    4 bytes    handle
     */

    xstream_seek(s, 12);
    xstream_wr_u32_le(s, 0); // map0
    xstream_seek(s, 4);
    xstream_wr_u32_le(s, 0); // map1

    xstream_wr_u32_le(s, control_code);

    xstream_wr_u32_le(s, send_bytes);

    val = send_bytes > 0 ? 1 : 0;
    xstream_wr_u32_le(s, val); // map2

    xstream_wr_u32_le(s, 0);
    xstream_wr_u32_le(s, recv_bytes);
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, sc_handle);

    if (send_bytes > 0)
    {
        xstream_wr_u32_le(s, send_bytes);
        out_uint8a(s, send_data, send_bytes);
    }

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
    return 0;
}

/**
 * Cancel any outstanding calls
 *****************************************************************************/
static int APP_CC
scard_send_Cancel(IRP* irp, tui32 context)
{
    /* see [MS-RDPESC] 3.1.4.27 */

    SMARTCARD*     sc;
    struct stream* s;
    int            bytes;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return 1;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_CANCEL)) == NULL)
    {
        log_error("scard_make_new_ioctl");
        return 1;
    }

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       16 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    context len
     * u32    4 bytes    context
     */

    xstream_seek(s, 16);
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, context);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
    return 0;
}

/**
 * Get reader attributes
 *****************************************************************************/
static int APP_CC
scard_send_GetAttrib(IRP* irp, tui32 sc_handle, READER_STATE* rs)
{
    /* see [MS-RDPESC] 2.2.2.21 */

    SMARTCARD*     sc;
    struct stream* s;
    int            bytes;

    if ((sc = smartcards[irp->scard_index]) == NULL)
    {
        log_error("smartcards[%d] is NULL", irp->scard_index);
        return 1;
    }

    if ((s = scard_make_new_ioctl(irp, SCARD_IOCTL_GETATTRIB)) == NULL)
    {
        log_error("scard_make_new_ioctl");
        return 1;
    }

    /*
     * command format
     *
     * ......
     *       20 bytes    padding
     * u32    4 bytes    len 8, LE, v1
     * u32    4 bytes    filler
     *       24 bytes    unused (s->p currently pointed here at unused[0])
     * u32    4 bytes    dwAttribId
     *        4 bytes    unused
     * u32    4 bytes    dwAttrLen
     *        8 bytes    unused
     * u32    4 bytes    handle len
     * u32    4 bytes    handle
     */

    xstream_seek(s, 24);
    xstream_wr_u32_le(s, rs->dwAttribId);
    xstream_wr_u32_le(s, 0);
    xstream_wr_u32_le(s, rs->dwAttrLen);
    xstream_seek(s, 8);
    xstream_wr_u32_le(s, 4);
    xstream_wr_u32_le(s, sc_handle);

    /* get stream len */
    bytes = xstream_len(s);

    /* InputBufferLength is number of bytes AFTER 20 byte padding */
    *(s->data + 28) = bytes - 56;

    /* send to client */
    send_channel_data(g_rdpdr_chan_id, s->data, bytes);
    xstream_free(s);
    return 0;
}

/******************************************************************************
**                                                                           **
**                    local callbacks into this module                       **
**                                                                           **
******************************************************************************/

/**
 *
 *****************************************************************************/
static void APP_CC
scard_handle_EstablishContext_Return(struct stream *s, IRP *irp,
                                     tui32 DeviceId, tui32 CompletionId,
                                     tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");
    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }
    if (IoStatus != 0)
    {
        log_error("failed to establish context - device not usable");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }
    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_establish_context_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void
scard_handle_ReleaseContext_Return(struct stream *s, IRP *irp,
                                   tui32 DeviceId, tui32 CompletionId,
                                   tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");
    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }
    if (IoStatus != 0)
    {
        log_error("ReleaseContext failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }
    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_release_context_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void
APP_CC scard_handle_IsContextValid_Return(struct stream *s, IRP *irp,
                                          tui32 DeviceId, tui32 CompletionId,
                                          tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("Error checking context validity");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_is_context_valid_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void APP_CC
scard_handle_ListReaders_Return(struct stream *s, IRP *irp,
                                tui32 DeviceId, tui32 CompletionId,
                                tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");
    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }
    if (IoStatus != 0)
    {
        log_error("failed to list readers");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }
    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_list_readers_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void
scard_handle_GetStatusChange_Return(struct stream *s, IRP *irp,
                                    tui32 DeviceId, tui32 CompletionId,
                                    tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");
    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }
    if (IoStatus != 0)
    {
        log_error("failed to get status change");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }
    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_get_status_change_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void
scard_handle_Connect_Return(struct stream *s, IRP *irp,
                            tui32 DeviceId, tui32 CompletionId,
                            tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("failed to connect");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);

    con = (struct trans *) (irp->user_data);
    scard_function_connect_return(con, s, len);
    devredir_irp_delete(irp);

    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void APP_CC
scard_handle_Reconnect_Return(struct stream *s, IRP *irp,
                              tui32 DeviceId, tui32 CompletionId,
                              tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("failed to reconnect");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_reconnect_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void
scard_handle_BeginTransaction_Return(struct stream *s, IRP *irp,
                                     tui32 DeviceId, tui32 CompletionId,
                                     tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("BeginTransaction failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_begin_transaction_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void
scard_handle_EndTransaction_Return(struct stream *s, IRP *irp,
                                   tui32 DeviceId, tui32 CompletionId,
                                   tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("EndTransaction failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_end_transaction_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void
scard_handle_Status_Return(struct stream *s, IRP *irp,
                           tui32 DeviceId, tui32 CompletionId,
                           tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("StatusCall failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_status_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void
scard_handle_Disconnect_Return(struct stream *s, IRP *irp,
                               tui32 DeviceId, tui32 CompletionId,
                               tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("Disconnect failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_disconnect_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void APP_CC
scard_handle_Transmit_Return(struct stream *s, IRP *irp, tui32 DeviceId,
                             tui32 CompletionId, tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("Transmit failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_transmit_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void APP_CC
scard_handle_Control_Return(struct stream *s, IRP *irp, tui32 DeviceId,
                            tui32 CompletionId,tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("Control failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_control_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void APP_CC
scard_handle_Cancel_Return(struct stream *s, IRP *irp, tui32 DeviceId,
                           tui32 CompletionId, tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("Cancel_call failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_cancel_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}

/**
 *
 *****************************************************************************/
static void APP_CC
scard_handle_GetAttrib_Return(struct stream *s, IRP *irp, tui32 DeviceId,
                              tui32 CompletionId, tui32 IoStatus)
{
    tui32 len;
    struct trans *con;

    log_debug("entered");

    /* sanity check */
    if ((DeviceId != irp->DeviceId) || (CompletionId != irp->CompletionId))
    {
        log_error("DeviceId/CompletionId do not match those in IRP");
        return;
    }

    if (IoStatus != 0)
    {
        log_error("GetAttrib_call failed");
        /* LK_TODO delete irp and smartcard entry */
        return;
    }

    /* get OutputBufferLen */
    xstream_rd_u32_le(s, len);
    con = (struct trans *) (irp->user_data);
    scard_function_get_attrib_return(con, s, len);
    devredir_irp_delete(irp);
    log_debug("leaving");
}