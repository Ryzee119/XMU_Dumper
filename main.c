#include <hal/debug.h>
#include <hal/video.h>
#include <nxdk/mount.h>
#include <assert.h>
#include "usbh_lib.h"
#include "usbh_msc.h"

 //D: is just in the same directory as the launching xbe
#define DUMP_NAME "D:\\xmu.bin"

//Pull from hal/debug.h
extern int nextRow;
extern int nextCol; 

void msc_connection_callback(MSC_T *msc_dev, int status)
{
    int ret;
    uint8_t *scsi_buff = usbh_alloc_mem(8);

    struct bulk_cb_wrap *cmd_blk = &msc_dev->cmd_blk;
    memset(cmd_blk, 0, sizeof(*cmd_blk));

    //Read the capacity of the drive
    cmd_blk->Flags = 0x80;
    cmd_blk->Length = 8;
    cmd_blk->CDB[0] = READ_CAPACITY;
    ret = run_scsi_command(msc_dev, (uint8_t *)scsi_buff, cmd_blk->Length, 1, 100);
    assert(ret == USBH_OK);

    //Setup device lun and sector size/count
    msc_dev->lun = 0;
    msc_dev->uTotalSectorN = (scsi_buff[0] << 24) | (scsi_buff[1] << 16) |
                             (scsi_buff[2] << 8) | scsi_buff[3];
    msc_dev->nSectorSize = (scsi_buff[4] << 24) | (scsi_buff[5] << 16) |
                           (scsi_buff[6] << 8) | scsi_buff[7];

    size_t num_sectors = msc_dev->uTotalSectorN;
    size_t capacity = num_sectors * 512 / 1024;
    size_t sector_size = msc_dev->nSectorSize;

    usbh_free_mem(scsi_buff, 8);

    debugPrint("USB Mass storage connected!\n");
    debugPrint("Capacity %dkB, Sector size: %d, Sector count: %d\n", capacity, sector_size, num_sectors);

    //Let's not dump anything too big.
    assert (capacity < 64*1024);

    FILE* xmu_file = fopen(DUMP_NAME, "wb");
    if (xmu_file == NULL)
    {
        debugPrint("Error: Could not open %s for write\n", DUMP_NAME);
        assert(0);
    }

    debugPrint("Opened %s OK for dumping\n", DUMP_NAME);
    debugPrint("Reading %d sectors to %s\n", num_sectors, DUMP_NAME);

    uint8_t *xmu_data = usbh_alloc_mem(sector_size);
    assert(xmu_data != NULL);

    int row = nextRow;
    int col = nextCol;

    for (int i = 0; i < num_sectors; i++)
    {
        ret = usbh_umas_read(msc_dev, i, 1, xmu_data);
        assert(ret == USBH_OK);
        size_t wb = fwrite(xmu_data, 1, sector_size, xmu_file);
        assert (wb == sector_size);
        debugPrint("%d/%d sectors\n", i, num_sectors);
        debugMoveCursor(col,row);
    }

    fclose(xmu_file);
    usbh_free_mem(xmu_data, sector_size);

    debugPrint("Complete. Saved to %s\n", DUMP_NAME);
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    //Mount E incase user wants to save it there
    nxMountDrive('E', "\\Device\\Harddisk0\\Partition1");

    usbh_core_init();
    usbh_umas_init();
    usbh_install_msc_conn_callback(msc_connection_callback, NULL);
    debugPrint("Insert your XMU into your controller\n");

    while (1) {
        usbh_pooling_hubs();
    }

    usbh_core_deinit();
    return 0;
}
