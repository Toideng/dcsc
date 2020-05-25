#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0x28950ef1, __VMLINUX_SYMBOL_STR(module_layout) },
	{ 0x15692c87, __VMLINUX_SYMBOL_STR(param_ops_int) },
	{ 0x37a0cba, __VMLINUX_SYMBOL_STR(kfree) },
	{ 0x5b2835a8, __VMLINUX_SYMBOL_STR(bus_unregister) },
	{ 0xb5a459dc, __VMLINUX_SYMBOL_STR(unregister_blkdev) },
	{ 0xd2b09ce5, __VMLINUX_SYMBOL_STR(__kmalloc) },
	{ 0x71a50dbc, __VMLINUX_SYMBOL_STR(register_blkdev) },
	{ 0xc3d55f5c, __VMLINUX_SYMBOL_STR(bus_register) },
	{ 0x58390d3, __VMLINUX_SYMBOL_STR(put_disk) },
	{ 0x95f82b97, __VMLINUX_SYMBOL_STR(del_gendisk) },
	{ 0x999e8297, __VMLINUX_SYMBOL_STR(vfree) },
	{ 0x61762346, __VMLINUX_SYMBOL_STR(blk_cleanup_queue) },
	{ 0xbc28fd2e, __VMLINUX_SYMBOL_STR(add_disk) },
	{ 0x7959fc3f, __VMLINUX_SYMBOL_STR(alloc_disk) },
	{ 0x177c57ca, __VMLINUX_SYMBOL_STR(blk_queue_logical_block_size) },
	{ 0x84daafd0, __VMLINUX_SYMBOL_STR(blk_init_queue) },
	{ 0xfb578fc5, __VMLINUX_SYMBOL_STR(memset) },
	{ 0xd6ee688f, __VMLINUX_SYMBOL_STR(vmalloc) },
	{ 0xdd08621f, __VMLINUX_SYMBOL_STR(driver_unregister) },
	{ 0x6a57436a, __VMLINUX_SYMBOL_STR(driver_create_file) },
	{ 0xaea8b4b4, __VMLINUX_SYMBOL_STR(driver_register) },
	{ 0x44f8da52, __VMLINUX_SYMBOL_STR(device_unregister) },
	{ 0xe4f79f4e, __VMLINUX_SYMBOL_STR(device_create_file) },
	{ 0xf283da2a, __VMLINUX_SYMBOL_STR(device_register) },
	{ 0xf01ecd60, __VMLINUX_SYMBOL_STR(dev_set_name) },
	{ 0xf0fdf6cb, __VMLINUX_SYMBOL_STR(__stack_chk_fail) },
	{ 0x71de9b3f, __VMLINUX_SYMBOL_STR(_copy_to_user) },
	{ 0x88db9f48, __VMLINUX_SYMBOL_STR(__check_object_size) },
	{ 0x27e1a049, __VMLINUX_SYMBOL_STR(printk) },
	{ 0x16305289, __VMLINUX_SYMBOL_STR(warn_slowpath_null) },
	{ 0x52385726, __VMLINUX_SYMBOL_STR(__blk_end_request_cur) },
	{ 0x69acdf38, __VMLINUX_SYMBOL_STR(memcpy) },
	{ 0x7cd8d75e, __VMLINUX_SYMBOL_STR(page_offset_base) },
	{ 0x97651e6c, __VMLINUX_SYMBOL_STR(vmemmap_base) },
	{ 0xb8c7ff88, __VMLINUX_SYMBOL_STR(current_task) },
	{ 0x605420f9, __VMLINUX_SYMBOL_STR(blk_fetch_request) },
	{ 0x28318305, __VMLINUX_SYMBOL_STR(snprintf) },
	{ 0xbdfb6dbb, __VMLINUX_SYMBOL_STR(__fentry__) },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "A031B6EDA04C14D0497EB7C");
MODULE_INFO(rhelversion, "7.8");
#ifdef RETPOLINE
	MODULE_INFO(retpoline, "Y");
#endif
#ifdef CONFIG_MPROFILE_KERNEL
	MODULE_INFO(mprofile, "Y");
#endif
