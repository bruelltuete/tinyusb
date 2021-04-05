/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 XMOS LIMITED
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if (TUSB_OPT_DEVICE_ENABLED && CFG_TUD_DFU_MODE) || (TUSB_OPT_DEVICE_ENABLED && CFG_TUD_DFU_RUNTIME_AND_MODE)

#include "dfu_mode_device.h"
#include "device/usbd_pvt.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+

//--------------------------------------------------------------------+
// INTERNAL OBJECT & FUNCTION DECLARATION
//--------------------------------------------------------------------+
typedef struct TU_ATTR_PACKED
{
    dfu_mode_device_status_t status;
    dfu_mode_state_t state;
    uint8_t attrs;
    bool blk_transfer_in_proc;

    uint8_t itf_num;
    uint16_t last_block_num;
    uint16_t last_transfer_len;
    CFG_TUSB_MEM_ALIGN uint8_t transfer_buf[CFG_TUD_DFU_TRANSFER_BUFFER_SIZE];
} dfu_mode_state_ctx_t;

// Only a single dfu state is allowed
CFG_TUSB_MEM_SECTION static dfu_mode_state_ctx_t _dfu_state_ctx;


static void dfu_req_dnload_setup(uint8_t rhport, tusb_control_request_t const * request);
static void dfu_req_getstatus_reply(uint8_t rhport, tusb_control_request_t const * request);
static uint16_t dfu_req_upload(uint8_t rhport, tusb_control_request_t const * request, uint16_t block_num, uint16_t wLength);
static void dfu_mode_req_dnload_reply(uint8_t rhport, tusb_control_request_t const * request);
static bool dfu_mode_state_machine(uint8_t rhport, tusb_control_request_t const * request);


//--------------------------------------------------------------------+
// USBD Driver API
//--------------------------------------------------------------------+
void dfu_mode_init(void)
{
  _dfu_state_ctx.state = APP_DETACH;    // After init, reset will occur.  We want to be in APP_DETACH to move to DFU_IDLE
  _dfu_state_ctx.status = DFU_STATUS_OK;
  _dfu_state_ctx.attrs = tud_dfu_mode_init_attrs_cb();
  _dfu_state_ctx.blk_transfer_in_proc = false;
  _dfu_state_ctx.last_block_num = 0;
  _dfu_state_ctx.last_transfer_len = 0;

  dfu_debug_print_context();
}

void dfu_mode_reset(uint8_t rhport)
{
  if (_dfu_state_ctx.state == APP_DETACH)
  {
      _dfu_state_ctx.state = DFU_IDLE;
  } else {
    if ( tud_dfu_mode_usb_reset_cb )
    {
      tud_dfu_mode_usb_reset_cb(rhport, &_dfu_state_ctx.state);
    } else {
      switch (_dfu_state_ctx.state)
      {
        case DFU_IDLE:
        case DFU_DNLOAD_SYNC:
        case DFU_DNBUSY:
        case DFU_DNLOAD_IDLE:
        case DFU_MANIFEST_SYNC:
        case DFU_MANIFEST:
        case DFU_MANIFEST_WAIT_RESET:
        case DFU_UPLOAD_IDLE:
        {
          _dfu_state_ctx.state = (tud_dfu_mode_firmware_valid_check_cb()) ?  APP_IDLE : DFU_ERROR;
        }
        break;

        case DFU_ERROR:
        default:
        {
          _dfu_state_ctx.state = APP_IDLE;
        }
        break;
      }
    }
  }

  if(_dfu_state_ctx.state == APP_IDLE)
  {
    tud_dfu_mode_reboot_to_rt_cb();
  }

  _dfu_state_ctx.status = DFU_STATUS_OK;
  _dfu_state_ctx.attrs = tud_dfu_mode_init_attrs_cb();
  _dfu_state_ctx.blk_transfer_in_proc = false;
  _dfu_state_ctx.last_block_num = 0;
  _dfu_state_ctx.last_transfer_len = 0;
  dfu_debug_print_context();
}

uint16_t dfu_mode_open(uint8_t rhport, tusb_desc_interface_t const * itf_desc, uint16_t max_len)
{
  (void) rhport;
  (void) max_len;

  // Ensure this is DFU Mode
  TU_VERIFY((itf_desc->bInterfaceSubClass == TUD_DFU_APP_SUBCLASS) &&
            (itf_desc->bInterfaceProtocol == DFU_PROTOCOL_DFU), 0);

  uint8_t const * p_desc = tu_desc_next( itf_desc );
  uint16_t drv_len = sizeof(tusb_desc_interface_t);

  if ( TUSB_DESC_FUNCTIONAL == tu_desc_type(p_desc) )
  {
    drv_len += tu_desc_len(p_desc);
    p_desc   = tu_desc_next(p_desc);
  }

  return drv_len;
}

// Invoked when a control transfer occurred on an interface of this class
// Driver response accordingly to the request and the transfer stage (setup/data/ack)
// return false to stall control endpoint (e.g unsupported request)
bool dfu_mode_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request)
{
  if ( (stage == CONTROL_STAGE_DATA) && (request->bRequest == DFU_DNLOAD_SYNC) )
  {
      dfu_mode_req_dnload_reply(rhport, request);
      return true;
  }

  // nothing to do with any other DATA or ACK stage
  if ( stage != CONTROL_STAGE_SETUP ) return true;

  TU_VERIFY(request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE);

  // dfu-util will try to claim the interface with SET_INTERFACE request before sending DFU request
  if ( TUSB_REQ_TYPE_STANDARD == request->bmRequestType_bit.type &&
       TUSB_REQ_SET_INTERFACE == request->bRequest )
  {
    tud_control_status(rhport, request);
    return true;
  }

  // Handle class request only from here
  TU_VERIFY(request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS);

  switch (request->bRequest)
  {
    case DFU_REQUEST_DETACH:
    case DFU_REQUEST_DNLOAD:
    case DFU_REQUEST_UPLOAD:
    case DFU_REQUEST_GETSTATUS:
    case DFU_REQUEST_CLRSTATUS:
    case DFU_REQUEST_GETSTATE:
    case DFU_REQUEST_ABORT:
    {
      return dfu_mode_state_machine(rhport, request);
    }
    break;

    default:
    {
      TU_LOG2("  DFU Nonstandard Request: %u\r\n", request->bRequest);
      return ( tud_dfu_mode_req_nonstandard_cb ) ? tud_dfu_mode_req_nonstandard_cb(rhport, stage, request) : false;
    }
    break;
  }

  return true;
}

static uint16_t dfu_req_upload(uint8_t rhport, tusb_control_request_t const * request, uint16_t block_num, uint16_t wLength)
{
  TU_VERIFY( wLength <= CFG_TUD_DFU_TRANSFER_BUFFER_SIZE);
  uint16_t retval = tud_dfu_mode_req_upload_data_cb(block_num, (uint8_t *)&_dfu_state_ctx.transfer_buf, wLength);
  tud_control_xfer(rhport, request, &_dfu_state_ctx.transfer_buf, retval);
  return retval;
}

static void dfu_req_getstatus_reply(uint8_t rhport, tusb_control_request_t const * request)
{
  dfu_status_req_payload_t resp;

  resp.bStatus = _dfu_state_ctx.status;
  if ( tud_dfu_mode_get_poll_timeout_cb )
  {
    tud_dfu_mode_get_poll_timeout_cb((uint8_t *)&resp.bwPollTimeout);
  } else {
    memset((uint8_t *)&resp.bwPollTimeout, 0x00, 3);
  }
  resp.bState = _dfu_state_ctx.state;
  resp.iString = ( tud_dfu_mode_get_status_desc_table_index_cb ) ? tud_dfu_mode_get_status_desc_table_index_cb() : 0;

  tud_control_xfer(rhport, request, &resp, sizeof(dfu_status_req_payload_t));
}

static void dfu_req_getstate_reply(uint8_t rhport, tusb_control_request_t const * request)
{
  tud_control_xfer(rhport, request, &_dfu_state_ctx.state, 1);
}

static void dfu_req_dnload_setup(uint8_t rhport, tusb_control_request_t const * request)
{
  _dfu_state_ctx.last_block_num = request->wValue;
  _dfu_state_ctx.last_transfer_len = request->wLength;
  // TODO: add "zero" copy mode so the buffer we read into can be provided by the user
  // if they wish, there still will be the internal control buffer copy to this buffer
  // but this mode would provide zero copy from the class driver to the application

  // setup for data phase
  tud_control_xfer(rhport, request, &_dfu_state_ctx.transfer_buf, request->wLength);
}

static void dfu_mode_req_dnload_reply(uint8_t rhport, tusb_control_request_t const * request)
{
  uint8_t bwPollTimeout[3] = {0,0,0};

  if ( tud_dfu_mode_get_poll_timeout_cb )
  {
    tud_dfu_mode_get_poll_timeout_cb((uint8_t *)&bwPollTimeout);
  }

  tud_dfu_mode_start_poll_timeout_cb((uint8_t *)&bwPollTimeout);

  // TODO: I want the real xferred len, not what is expected.  May need to change usbd.c to do this.
  tud_dfu_mode_req_dnload_data_cb(_dfu_state_ctx.last_block_num, (uint8_t *)&_dfu_state_ctx.transfer_buf, _dfu_state_ctx.last_transfer_len);
  _dfu_state_ctx.blk_transfer_in_proc = false;

  _dfu_state_ctx.last_block_num = 0;
  _dfu_state_ctx.last_transfer_len = 0;
}

void tud_dfu_mode_poll_timeout_done()
{
  if (_dfu_state_ctx.state == DFU_DNBUSY)
  {
    _dfu_state_ctx.state = DFU_DNLOAD_SYNC;
  } else if (_dfu_state_ctx.state == DFU_MANIFEST)
  {
    _dfu_state_ctx.state = ((_dfu_state_ctx.attrs & DFU_FUNC_ATTR_MANIFESTATION_TOLERANT_BITMASK) == 0)
                           ? DFU_MANIFEST_WAIT_RESET : DFU_MANIFEST_SYNC;
  }
}

static bool dfu_mode_state_machine(uint8_t rhport, tusb_control_request_t const * request)
{
  TU_LOG2("  DFU Request: %s\r\n", tu_lookup_find(&_dfu_request_table, request->bRequest));
  TU_LOG2("  DFU State Machine: %s\r\n", tu_lookup_find(&_dfu_mode_state_table, _dfu_state_ctx.state));

  switch (_dfu_state_ctx.state)
  {
    case DFU_IDLE:
    {
      switch (request->bRequest)
      {
        case DFU_REQUEST_DNLOAD:
        {
          if( ((_dfu_state_ctx.attrs & DFU_FUNC_ATTR_CAN_DOWNLOAD_BITMASK) != 0)
              && (request->wLength > 0) )
          {
            _dfu_state_ctx.state = DFU_DNLOAD_SYNC;
            _dfu_state_ctx.blk_transfer_in_proc = true;
            dfu_req_dnload_setup(rhport, request);
          } else {
            _dfu_state_ctx.state = DFU_ERROR;
          }
        }
        break;

        case DFU_REQUEST_UPLOAD:
        {
          if( ((_dfu_state_ctx.attrs & DFU_FUNC_ATTR_CAN_UPLOAD_BITMASK) != 0) )
          {
            _dfu_state_ctx.state = DFU_UPLOAD_IDLE;
            dfu_req_upload(rhport, request, request->wValue, request->wLength);
          } else {
            _dfu_state_ctx.state = DFU_ERROR;
          }
        }
        break;

        case DFU_REQUEST_GETSTATUS:
        {
          dfu_req_getstatus_reply(rhport, request);
        }
        break;

        case DFU_REQUEST_GETSTATE:
        {
          dfu_req_getstate_reply(rhport, request);
        }
        break;

        case DFU_REQUEST_ABORT:
        {
          ; // do nothing, but don't stall so continue on
        }
        break;

        default:
        {
          _dfu_state_ctx.state = DFU_ERROR;
          return false;  // stall on all other requests
        }
        break;
      }
    }
    break;

    case DFU_DNLOAD_SYNC:
    {
      switch (request->bRequest)
      {
        case DFU_REQUEST_GETSTATUS:
        {
          if ( _dfu_state_ctx.blk_transfer_in_proc )
          {
            _dfu_state_ctx.state = DFU_DNBUSY;
            dfu_req_getstatus_reply(rhport, request);
          } else {
            _dfu_state_ctx.state = DFU_DNLOAD_IDLE;
            dfu_req_getstatus_reply(rhport, request);
          }
        }
        break;

        case DFU_REQUEST_GETSTATE:
        {
          dfu_req_getstate_reply(rhport, request);
        }
        break;

        default:
        {
          _dfu_state_ctx.state = DFU_ERROR;
          return false;  // stall on all other requests
        }
        break;
      }
    }
    break;

    case DFU_DNBUSY:
    {
      switch (request->bRequest)
      {
        default:
        {
          _dfu_state_ctx.state = DFU_ERROR;
          return false;  // stall on all other requests
        }
        break;
      }
    }
    break;

    case DFU_DNLOAD_IDLE:
    {
        switch (request->bRequest)
        {
          case DFU_REQUEST_DNLOAD:
          {
            if( ((_dfu_state_ctx.attrs & DFU_FUNC_ATTR_CAN_DOWNLOAD_BITMASK) != 0)
                && (request->wLength > 0) )
            {
              _dfu_state_ctx.state = DFU_DNLOAD_SYNC;
              _dfu_state_ctx.blk_transfer_in_proc = true;

              dfu_req_dnload_setup(rhport, request);
            } else {
              if ( tud_dfu_mode_device_data_done_check_cb() )
              {
                _dfu_state_ctx.state = DFU_MANIFEST_SYNC;
                tud_control_status(rhport, request);
              } else {
                _dfu_state_ctx.state = DFU_ERROR;
                return false;  // stall
              }
            }
          }
          break;

          case DFU_REQUEST_GETSTATUS:
          {
            dfu_req_getstatus_reply(rhport, request);
          }
          break;

          case DFU_REQUEST_GETSTATE:
          {
            dfu_req_getstate_reply(rhport, request);
          }
          break;

          case DFU_REQUEST_ABORT:
          {
            if ( tud_dfu_mode_abort_cb )
            {
              tud_dfu_mode_abort_cb();
            }
            _dfu_state_ctx.state = DFU_IDLE;
          }
          break;

          default:
          {
            _dfu_state_ctx.state = DFU_ERROR;
            return false;  // stall on all other requests
          }
          break;
        }
    }
    break;

    case DFU_MANIFEST_SYNC:
    {
      switch (request->bRequest)
      {
        case DFU_REQUEST_GETSTATUS:
        {
          if ((_dfu_state_ctx.attrs & DFU_FUNC_ATTR_MANIFESTATION_TOLERANT_BITMASK) == 0)
          {
            _dfu_state_ctx.state = DFU_MANIFEST;
            dfu_req_getstatus_reply(rhport, request);
          } else {
            if ( tud_dfu_mode_firmware_valid_check_cb() )
            {
              _dfu_state_ctx.state = DFU_IDLE;
            }
            dfu_req_getstatus_reply(rhport, request);
          }
        }
        break;

        case DFU_REQUEST_GETSTATE:
        {
          dfu_req_getstate_reply(rhport, request);
        }
        break;

        default:
        {
          _dfu_state_ctx.state = DFU_ERROR;
          return false;  // stall on all other requests
        }
        break;
      }
    }
    break;

    case DFU_MANIFEST:
    {
      switch (request->bRequest)
      {
        default:
        {
          return false;  // stall on all other requests
        }
        break;
      }
    }
    break;

    case DFU_MANIFEST_WAIT_RESET:
    {
      // technically we should never even get here, but we will handle it just in case
      TU_LOG2("  DFU was in DFU_MANIFEST_WAIT_RESET and got unexpected request: %u\r\n", request->bRequest);
      switch (request->bRequest)
      {
        default:
        {
          return false;  // stall on all other requests
        }
        break;
      }
    }
    break;

    case DFU_UPLOAD_IDLE:
    {
      switch (request->bRequest)
      {
        case DFU_REQUEST_UPLOAD:
        {
          if (dfu_req_upload(rhport, request, request->wValue, request->wLength) != request->wLength)
          {
            _dfu_state_ctx.state = DFU_IDLE;
          }
        }
        break;

        case DFU_REQUEST_GETSTATUS:
        {
          dfu_req_getstatus_reply(rhport, request);
        }
        break;

        case DFU_REQUEST_GETSTATE:
        {
          dfu_req_getstate_reply(rhport, request);
        }
        break;

        case DFU_REQUEST_ABORT:
        {
          if (tud_dfu_mode_abort_cb)
          {
            tud_dfu_mode_abort_cb();
          }
          _dfu_state_ctx.state = DFU_IDLE;
        }
        break;

        default:
        {
          return false;  // stall on all other requests
        }
        break;
      }
    }
    break;

    case DFU_ERROR:
    {
      switch (request->bRequest)
      {
        case DFU_REQUEST_GETSTATUS:
        {
          dfu_req_getstatus_reply(rhport, request);
        }
        break;

        case DFU_REQUEST_CLRSTATUS:
        {
          _dfu_state_ctx.state = DFU_IDLE;
        }
        break;

        case DFU_REQUEST_GETSTATE:
        {
          dfu_req_getstate_reply(rhport, request);
        }
        break;

        default:
        {
          return false;  // stall on all other requests
        }
        break;
      }
    }
    break;

    default:
      _dfu_state_ctx.state = DFU_ERROR;
      TU_LOG2("  DFU ERROR: Unexpected state\r\nStalling control pipe\r\n");
      return false;  // Unexpected state, stall and change to error
  }

  return true;
}


#endif
