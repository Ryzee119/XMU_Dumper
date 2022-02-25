XBE_TITLE = XMU\ Dumper
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(CURDIR)/main.c
SRCS += $(CURDIR)/src_xmu/msc_driver.c $(CURDIR)/src_xmu/msc_xfer.c
CFLAGS += -Isrc_xmu
include $(NXDK_DIR)/Makefile
