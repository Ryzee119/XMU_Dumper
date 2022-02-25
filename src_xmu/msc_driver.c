/**************************************************************************//**
 * @file     msc_driver.c
 * @version  V1.00
 * $Revision: 4 $
 * $Date: 14/10/07 4:33p $
 * @brief    Lightweight USB mass storage class driver
 *
 * @note
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2017 Nuvoton Technology Corp. All rights reserved.
 * Copyright (C) 2021 Ryan Wendland (remove FATFS requirement)
*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "N9H30.h"
#include "usb.h"
#include "usbh_lib.h"
#include "usbh_msc.h"


/// @cond HIDDEN_SYMBOLS


MSC_T  *g_msc_list;       /* Global list of Mass Storage Class device. A multi-lun device can have
                             several instances appeared with different lun. */

static MSC_CONN_FUNC *g_msc_conn_func, *g_msc_disconn_func;

static void msc_list_add(MSC_T *msc)
{
    if (g_msc_list == NULL)
    {
        msc->next = NULL;
        g_msc_list = msc;
    }
    else
    {
        msc->next = g_msc_list;
        g_msc_list = msc;
    }
}

static void msc_list_remove(MSC_T *msc)
{
    MSC_T   *p;

    if (g_msc_list == msc)
    {
        g_msc_list = msc->next;
    }
    else
    {
        p = g_msc_list;
        while ((p->next != msc) && (p->next != NULL))
        {
            p = p->next;
        }

        if (p->next == msc)
        {
            p->next = msc->next;
        }
    }
}


static void get_max_lun(MSC_T *msc)
{
    UDEV_T    *udev = msc->iface->udev;
    uint32_t  read_len;
    uint8_t   *nbuff;
    int       ret;

    msc->max_lun = 0;
    nbuff = usbh_alloc_mem(2);

    /*------------------------------------------------------------------------------------*/
    /* Issue GET MAXLUN MSC class command to get the maximum lun number                   */
    /*------------------------------------------------------------------------------------*/
    ret = usbh_ctrl_xfer(udev, REQ_TYPE_IN | REQ_TYPE_CLASS_DEV | REQ_TYPE_TO_IFACE,
                         0xFE, 0, 0, 1, nbuff, &read_len, 200);
    if (ret < 0)
    {
        msc_debug_msg("Get Max Lun command failed! Assign 0...\n");
        msc->max_lun = 0;
        if (ret == USBH_ERR_STALL)
            usbh_clear_halt(udev, 0);
        return;
    }
    msc->max_lun = nbuff[0];
    msc_debug_msg("Max lun is %d\n", msc->max_lun);
    usbh_free_mem(nbuff, 2);
}

void msc_reset(MSC_T *msc)
{
    UDEV_T    *udev = msc->iface->udev;
    uint32_t  read_len;
    int       ret;

    msc_debug_msg("Reset MSC device...\n");

    ret = usbh_ctrl_xfer(udev, REQ_TYPE_OUT | REQ_TYPE_CLASS_DEV | REQ_TYPE_TO_IFACE,
                         0xFF, 0, msc->iface->if_num, 0, NULL, &read_len, 100);
    if (ret < 0)
    {
        msc_debug_msg("UAMSS reset request failed!\n");
    }

    usbh_clear_halt(udev, msc->ep_bulk_out->bEndpointAddress);
    usbh_clear_halt(udev, msc->ep_bulk_in->bEndpointAddress);
}

static int  msc_inquiry(MSC_T *msc)
{
    struct bulk_cb_wrap  *cmd_blk = &msc->cmd_blk;         /* MSC Bulk-only command block   */
    uint8_t   *scsi_buff;
    int  ret;

    msc_debug_msg("INQUIRY...\n");
    memset(cmd_blk, 0, sizeof(struct bulk_cb_wrap));

    cmd_blk->Flags   = 0x80;
    cmd_blk->Length  = 6;
    cmd_blk->CDB[0]  = INQUIRY;         /* INQUIRY */
    cmd_blk->CDB[1]  = msc->lun << 5;
    cmd_blk->CDB[4]  = 36;

    scsi_buff = msc->scsi_buff;
    ret = run_scsi_command(msc, scsi_buff, 36, 1, 100);
    if (ret < 0)
    {
        msc_debug_msg("INQUIRY command failed. [%d]\n", ret);
        return ret;
    }
    else
    {
        msc_debug_msg("INQUIRY command success.\n");
    }
    return ret;
}

static int  msc_request_sense(MSC_T *msc)
{
    struct bulk_cb_wrap  *cmd_blk = &msc->cmd_blk;
    uint8_t   *scsi_buff;
    int       ret;

    msc_debug_msg("REQUEST_SENSE...\n");
    memset(cmd_blk, 0, sizeof(*cmd_blk));

    cmd_blk->Flags   = 0x80;
    cmd_blk->Length  = 6;
    cmd_blk->CDB[0]  = REQUEST_SENSE;
    cmd_blk->CDB[1]  = msc->lun << 5;
    cmd_blk->CDB[4]  = 18;

    scsi_buff = msc->scsi_buff;
    ret = run_scsi_command(msc, scsi_buff, 18, 1, 100);
    if (ret < 0)
    {
        msc_debug_msg("REQUEST_SENSE command failed.\n");
        if (ret == USBH_ERR_STALL)
            msc_reset(msc);
        return ret;
    }
    else
    {
        msc_debug_msg("REQUEST_SENSE command success.\n");
        if (scsi_buff[2] != 0x6)
        {
            msc_debug_msg("Device is still not attention. 0x%x\n", scsi_buff[2]);
            return -1;
        }
    }
    return ret;
}

static int  msc_test_unit_ready(MSC_T *msc)
{
    struct bulk_cb_wrap  *cmd_blk = &msc->cmd_blk;         /* MSC Bulk-only command block   */
    int  ret;

    msc_debug_msg("TEST_UNIT_READY...\n");
    memset(cmd_blk, 0, sizeof(*cmd_blk));

    cmd_blk->Flags   = 0x80;
    cmd_blk->Length  = 6;
    cmd_blk->CDB[0]  = TEST_UNIT_READY;
    cmd_blk->CDB[1]  = msc->lun << 5;

    ret = run_scsi_command(msc, msc->scsi_buff, 0, 1, 100);
    if (ret < 0)
    {
        if (ret == USBH_ERR_STALL)
            msc_reset(msc);
        return ret;
    }
    else
    {
        msc_debug_msg("TEST_UNIT_READY command success.\n");
    }
    return ret;
}

/**
  * @brief       Read a number of contiguous sectors from mass storage device.
  *
  * @param[in]   msc       MSC device pointer
  * @param[in]   sec_no    Sector number of the start secotr.
  * @param[in]   sec_cnt   Number of sectors to be read.
  * @param[out]  buff      Memory buffer to store data read from disk.
  *
  * @retval      0       Success
  * @retval      - \ref UMAS_ERR_DRIVE_NOT_FOUND   There's no mass storage device mounted to this volume.
  * @retval      - \ref UMAS_ERR_IO      Failed to read disk.
  */
int  usbh_umas_read(MSC_T *msc, uint32_t sec_no, int sec_cnt, uint8_t *buff)
{
    struct bulk_cb_wrap  *cmd_blk;         /* MSC Bulk-only command block   */
    int   ret;

    msc_debug_msg("usbh_umas_read - %d, %d, 0x%x\n", sec_no, sec_cnt, (int)buff);

    if (msc == NULL)
        return UMAS_ERR_DRIVE_NOT_FOUND;

    cmd_blk = &msc->cmd_blk;

    //msc_debug_msg("read sector 0x%x\n", sec_no);
    memset(cmd_blk, 0, sizeof(*cmd_blk));

    cmd_blk->Flags   = 0x80;
    cmd_blk->Length  = 10;
    cmd_blk->CDB[0]  = READ_10;
    cmd_blk->CDB[1]  = msc->lun << 5;
    cmd_blk->CDB[2]  = (sec_no >> 24) & 0xFF;
    cmd_blk->CDB[3]  = (sec_no >> 16) & 0xFF;
    cmd_blk->CDB[4]  = (sec_no >> 8) & 0xFF;
    cmd_blk->CDB[5]  = sec_no & 0xFF;
    cmd_blk->CDB[7]  = (sec_cnt >> 8) & 0xFF;
    cmd_blk->CDB[8]  = sec_cnt & 0xFF;

    ret = run_scsi_command(msc, buff, sec_cnt * 512, 1, 500);
    if (ret != 0)
    {
        msc_debug_msg("usbh_umas_read failed! [%d]\n", ret);
        return ret;
    }
    return 0;
}

/**
  * @brief       Write a number of contiguous sectors to mass storage device.
  *
  * @param[in]   msc       HID device pointer
  * @param[in]   sec_no    Sector number of the start secotr.
  * @param[in]   sec_cnt   Number of sectors to be written.
  * @param[in]   buff      Memory buffer hold the data to be written..
  *
  * @retval      0       Success
  * @retval      - \ref UMAS_ERR_DRIVE_NOT_FOUND   There's no mass storage device mounted to this volume.
  * @retval      - \ref UMAS_ERR_IO      Failed to write disk.
  */
int  usbh_umas_write(MSC_T *msc, uint32_t sec_no, int sec_cnt, uint8_t *buff)
{
    struct bulk_cb_wrap  *cmd_blk;         /* MSC Bulk-only command block   */
    int   ret;

    //msc_debug_msg("usbh_umas_write - %d, %d\n", sec_no, sec_cnt);

    if (msc == NULL)
        return UMAS_ERR_DRIVE_NOT_FOUND;

    cmd_blk = &msc->cmd_blk;
    memset((uint8_t *)&(msc->cmd_blk), 0, sizeof(msc->cmd_blk));

    cmd_blk->Flags   = 0;
    cmd_blk->Length  = 10;
    cmd_blk->CDB[0]  = WRITE_10;
    cmd_blk->CDB[1]  = msc->lun << 5;
    cmd_blk->CDB[2]  = (sec_no >> 24) & 0xFF;
    cmd_blk->CDB[3]  = (sec_no >> 16) & 0xFF;
    cmd_blk->CDB[4]  = (sec_no >> 8) & 0xFF;
    cmd_blk->CDB[5]  = sec_no & 0xFF;
    cmd_blk->CDB[7]  = (sec_cnt >> 8) & 0xFF;
    cmd_blk->CDB[8]  = sec_cnt & 0xFF;

    ret = run_scsi_command(msc, buff, sec_cnt * 512, 0, 500);
    if (ret < 0)
    {
        msc_debug_msg("usbh_umas_write failed!\n");
        return UMAS_ERR_IO;
    }
    return 0;
}

/**
 *  @brief    Reset a connected USB mass storage device.
 *  @param[in] msc        MSC device pointer
 *  @retval    0          Succes
 *  @retval    Otherwise  Failed
 */
int  usbh_umas_reset_disk(MSC_T *msc)
{
    UDEV_T     *udev;

    sysprintf("usbh_umas_reset_disk ...\n");

    if (msc == NULL)
        return UMAS_ERR_DRIVE_NOT_FOUND;

    udev = msc->iface->udev;

    usbh_reset_device(udev);

    return 0;
}

static int  umas_init_device(MSC_T *msc)
{
    MSC_T     *try_msc = msc;
    struct bulk_cb_wrap  *cmd_blk;         /* MSC Bulk-only command block   */
    int       retries, lun;
    int8_t    bHasMedia = 0;
    uint8_t   *scsi_buff;
    int       ret = USBH_ERR_NOT_FOUND;
    lun = 0;

    msc_list_add(msc);
    if (g_msc_conn_func)
        g_msc_conn_func(msc, 0);

    return USBH_OK;
}

static int msc_probe(IFACE_T *iface)
{
    ALT_IFACE_T   *aif = iface->aif;
    DESC_IF_T     *ifd;
    MSC_T         *msc;
    int           i;

    ifd = aif->ifd;

    /* Is this interface mass stroage class? */
    if (ifd->bInterfaceClass != USB_CLASS_MASS_STORAGE)
        return USBH_ERR_NOT_MATCHED;

    /* Is supported sub-class? */
    if ((ifd->bInterfaceSubClass != MSC_SCLASS_SCSI) && (ifd->bInterfaceSubClass != MSC_SCLASS_8070) &&
            (ifd->bInterfaceSubClass != MSC_SCLASS_RBC) && (ifd->bInterfaceSubClass != 0x42)) //XMU has non standard subclass. Its still SCSI though :)
        return USBH_ERR_NOT_SUPPORTED;

    /* Is bulk-only protocl? */
    if (ifd->bInterfaceProtocol != MSC_SPROTO_BULK)
    {
        msc_debug_msg("Not bulk-only MSC device!\n");
        return USBH_ERR_NOT_SUPPORTED;
    }

    msc = (MSC_T *)usbh_alloc_mem(sizeof(*msc));
    if (msc == NULL)
        return USBH_ERR_MEMORY_OUT;

    msc->scsi_buff = usbh_alloc_mem(SCSI_BUFF_LEN);
    if (msc->scsi_buff == NULL)
        return USBH_ERR_MEMORY_OUT;

    msc->uid = get_ticks();

    /* Find the bulk in and out endpoints */
    for (i = 0; i < aif->ifd->bNumEndpoints; i++)
    {
        if ((aif->ep[i].bmAttributes & EP_ATTR_TT_MASK) == EP_ATTR_TT_BULK)
        {
            if ((aif->ep[i].bEndpointAddress & EP_ADDR_DIR_MASK) == EP_ADDR_DIR_IN)
                msc->ep_bulk_in = &aif->ep[i];
            else
                msc->ep_bulk_out = &aif->ep[i];
        }
    }

    if ((msc->ep_bulk_in == NULL) || (msc->ep_bulk_out == NULL))
    {
        usbh_free_mem(msc->scsi_buff, SCSI_BUFF_LEN);
        usbh_free_mem(msc, sizeof(*msc));
        return USBH_ERR_NOT_EXPECTED;
    }

    msc->iface = iface;

    msc_debug_msg("USB Mass Storage device found. Iface:%d, Alt Iface:%d, bep_in:0x%x, bep_out:0x%x\n", ifd->bInterfaceNumber, ifd->bAlternateSetting, msc->ep_bulk_in->bEndpointAddress, msc->ep_bulk_out->bEndpointAddress);

    get_max_lun(msc);

    return umas_init_device(msc);
}

static void msc_disconnect(IFACE_T *iface)
{
    int    i;
    MSC_T  *msc_p, *msc;

    /*
     *  Remove any hardware EP/QH from Host Controller hardware list.
     *  This will finally result in all transfers aborted.
     */
    for (i = 0; i < iface->aif->ifd->bNumEndpoints; i++)
    {
        iface->udev->hc_driver->quit_xfer(NULL, &(iface->aif->ep[i]));
    }

    /*
     *  unmount drive and remove it from MSC device list
     */
    msc = g_msc_list;
    while (msc != NULL)
    {
        msc_p = msc->next;
        if (msc->iface == iface)
        {

            if (g_msc_disconn_func)
                g_msc_disconn_func(msc, 0);

            msc_list_remove(msc);
            if (msc->scsi_buff)
                usbh_free_mem(msc->scsi_buff, SCSI_BUFF_LEN);
            usbh_free_mem(msc, sizeof(*msc));
        }
        msc = msc_p;
    }
}

/**
  * @brief    Install msc connect and disconnect callback function.
  *
  * @param[in]  conn_func       msc connect callback function.
  * @param[in]  disconn_func    msc disconnect callback function.
  * @return     None.
  */
void usbh_install_msc_conn_callback(MSC_CONN_FUNC *conn_func, MSC_CONN_FUNC *disconn_func)
{
    g_msc_conn_func = conn_func;
    g_msc_disconn_func = disconn_func;
}

UDEV_DRV_T  msc_driver =
{
    msc_probe,
    msc_disconnect,
    NULL,
    NULL
};


/// @endcond HIDDEN_SYMBOLS


/**
  * @brief       Register and initialize USB Host Mass Storage driver.
  *
  * @retval      0    Success.
  * @retval      1    Failed.
  */
int  usbh_umas_init(void)
{
    g_msc_list = NULL;
    g_msc_conn_func = NULL;
    g_msc_disconn_func = NULL;
    return usbh_register_driver(&msc_driver);
}

/**
 *  @brief   Get a list of currently connected USB MSC devices.
 *  @return  A list MSC_T pointer reference to connected MSC devices.
 *  @retval  NULL       There's no MSC device found.
 *  @retval  Otherwise  A list of connected MSC devices.
 *
 *  The MSC devices are chained by the "next" member of MSC_T.
 */
MSC_T * usbh_msc_get_device_list(void)
{
    return g_msc_list;
}


/*** (C) COPYRIGHT 2017 Nuvoton Technology Corp. ***/


