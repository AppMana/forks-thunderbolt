#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

KSYMTAB_FUNC(__tb_ring_enqueue, "_gpl", "");
KSYMTAB_FUNC(tb_ring_poll, "_gpl", "");
KSYMTAB_FUNC(tb_ring_poll_complete, "_gpl", "");
KSYMTAB_FUNC(tb_ring_throttling, "_gpl", "");
KSYMTAB_FUNC(tb_ring_alloc_tx, "_gpl", "");
KSYMTAB_FUNC(tb_ring_alloc_rx, "_gpl", "");
KSYMTAB_FUNC(tb_ring_start, "_gpl", "");
KSYMTAB_FUNC(tb_ring_stop, "_gpl", "");
KSYMTAB_FUNC(tb_ring_free, "_gpl", "");
KSYMTAB_FUNC(tb_property_create_dir, "_gpl", "");
KSYMTAB_FUNC(tb_property_free_dir, "_gpl", "");
KSYMTAB_FUNC(tb_property_add_immediate, "_gpl", "");
KSYMTAB_FUNC(tb_property_add_data, "_gpl", "");
KSYMTAB_FUNC(tb_property_add_text, "_gpl", "");
KSYMTAB_FUNC(tb_property_add_dir, "_gpl", "");
KSYMTAB_FUNC(tb_property_remove, "_gpl", "");
KSYMTAB_FUNC(tb_property_find, "_gpl", "");
KSYMTAB_FUNC(tb_property_get_next, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_response, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_request, "_gpl", "");
KSYMTAB_FUNC(tb_register_protocol_handler, "_gpl", "");
KSYMTAB_FUNC(tb_unregister_protocol_handler, "_gpl", "");
KSYMTAB_FUNC(tb_register_service_driver, "_gpl", "");
KSYMTAB_FUNC(tb_unregister_service_driver, "_gpl", "");
KSYMTAB_DATA(tb_service_type, "_gpl", "");
KSYMTAB_DATA(tb_xdomain_type, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_lane_bonding_enable, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_lane_bonding_disable, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_alloc_in_hopid, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_alloc_out_hopid, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_release_in_hopid, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_release_out_hopid, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_enable_paths, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_disable_paths, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_find_by_uuid, "_gpl", "");
KSYMTAB_FUNC(tb_xdomain_find_by_route, "_gpl", "");
KSYMTAB_FUNC(tb_register_property_dir, "_gpl", "");
KSYMTAB_FUNC(tb_unregister_property_dir, "_gpl", "");
KSYMTAB_FUNC(usb4_usb3_port_match, "_gpl", "");

SYMBOL_CRC(__tb_ring_enqueue, 0x9c662f86, "_gpl");
SYMBOL_CRC(tb_ring_poll, 0x2b76adaa, "_gpl");
SYMBOL_CRC(tb_ring_poll_complete, 0x751428af, "_gpl");
SYMBOL_CRC(tb_ring_throttling, 0xe3d4814d, "_gpl");
SYMBOL_CRC(tb_ring_alloc_tx, 0x72b9e7b1, "_gpl");
SYMBOL_CRC(tb_ring_alloc_rx, 0x104505a0, "_gpl");
SYMBOL_CRC(tb_ring_start, 0x751428af, "_gpl");
SYMBOL_CRC(tb_ring_stop, 0x751428af, "_gpl");
SYMBOL_CRC(tb_ring_free, 0x751428af, "_gpl");
SYMBOL_CRC(tb_property_create_dir, 0x41240aa5, "_gpl");
SYMBOL_CRC(tb_property_free_dir, 0xfa1297b2, "_gpl");
SYMBOL_CRC(tb_property_add_immediate, 0xd1ddcf19, "_gpl");
SYMBOL_CRC(tb_property_add_data, 0xa4eaa7b1, "_gpl");
SYMBOL_CRC(tb_property_add_text, 0x8d2b2ba2, "_gpl");
SYMBOL_CRC(tb_property_add_dir, 0x59051fab, "_gpl");
SYMBOL_CRC(tb_property_remove, 0xf50c6edf, "_gpl");
SYMBOL_CRC(tb_property_find, 0x12e9551a, "_gpl");
SYMBOL_CRC(tb_property_get_next, 0x72750d23, "_gpl");
SYMBOL_CRC(tb_xdomain_response, 0xc61bbb47, "_gpl");
SYMBOL_CRC(tb_xdomain_request, 0xf46066ae, "_gpl");
SYMBOL_CRC(tb_register_protocol_handler, 0xafb85506, "_gpl");
SYMBOL_CRC(tb_unregister_protocol_handler, 0xb760ce2d, "_gpl");
SYMBOL_CRC(tb_register_service_driver, 0x931419a6, "_gpl");
SYMBOL_CRC(tb_unregister_service_driver, 0xf74a5c79, "_gpl");
SYMBOL_CRC(tb_service_type, 0xf839743b, "_gpl");
SYMBOL_CRC(tb_xdomain_type, 0xf839743b, "_gpl");
SYMBOL_CRC(tb_xdomain_lane_bonding_enable, 0xce1b1116, "_gpl");
SYMBOL_CRC(tb_xdomain_lane_bonding_disable, 0x0fa440fc, "_gpl");
SYMBOL_CRC(tb_xdomain_alloc_in_hopid, 0x02cc4082, "_gpl");
SYMBOL_CRC(tb_xdomain_alloc_out_hopid, 0x02cc4082, "_gpl");
SYMBOL_CRC(tb_xdomain_release_in_hopid, 0xb86c2a81, "_gpl");
SYMBOL_CRC(tb_xdomain_release_out_hopid, 0xb86c2a81, "_gpl");
SYMBOL_CRC(tb_xdomain_enable_paths, 0xc7c215d6, "_gpl");
SYMBOL_CRC(tb_xdomain_disable_paths, 0xc7c215d6, "_gpl");
SYMBOL_CRC(tb_xdomain_find_by_uuid, 0xf0e1fc0c, "_gpl");
SYMBOL_CRC(tb_xdomain_find_by_route, 0xbf89832c, "_gpl");
SYMBOL_CRC(tb_register_property_dir, 0x1fab9beb, "_gpl");
SYMBOL_CRC(tb_unregister_property_dir, 0xf0995667, "_gpl");
SYMBOL_CRC(usb4_usb3_port_match, 0x297f10a3, "_gpl");

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x21f931bb, "add_uevent_var" },
	{ 0x9dd4105e, "free_irq" },
	{ 0xd98156e2, "ida_alloc_range" },
	{ 0x827878ff, "pcie_capability_read_word" },
	{ 0x7e2232fb, "ioread32" },
	{ 0xa61fd7aa, "__check_object_size" },
	{ 0x95305a11, "pci_scan_child_bus" },
	{ 0x63f4e388, "pci_find_ext_capability" },
	{ 0x52b27c53, "is_acpi_device_node" },
	{ 0x0c60341d, "device_set_wakeup_capable" },
	{ 0x97195299, "bpf_trace_run4" },
	{ 0x740648c4, "ida_destroy" },
	{ 0xba0a7756, "devm_request_threaded_irq" },
	{ 0x4d8419c6, "param_ops_uint" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0x57860fb4, "wait_for_completion_timeout" },
	{ 0x12f6a6e7, "devm_kmalloc" },
	{ 0xfad8f384, "iowrite32" },
	{ 0x42e82979, "acpi_walk_namespace" },
	{ 0x534ed5f3, "__msecs_to_jiffies" },
	{ 0xd710adbf, "__kmalloc_noprof" },
	{ 0x41495f0d, "strim" },
	{ 0x143526e8, "dev_set_name" },
	{ 0xa6c3b74d, "ktime_get_mono_fast_ns" },
	{ 0x64ed6ec2, "trace_seq_printf" },
	{ 0x0845aed0, "pci_alloc_irq_vectors" },
	{ 0x40a621c5, "snprintf" },
	{ 0x65026e43, "complete" },
	{ 0x49733ad6, "queue_work_on" },
	{ 0xabdb103e, "trace_raw_output_prep" },
	{ 0x7a4352ad, "pci_dev_put" },
	{ 0xd4a4112c, "device_unregister" },
	{ 0x6363ea20, "pm_runtime_set_autosuspend_delay" },
	{ 0x60c9c0b3, "__init_swait_queue_head" },
	{ 0xfccc5035, "__trace_trigger_soft_disabled" },
	{ 0xc87f4bab, "finish_wait" },
	{ 0x5913fe24, "nvmem_unregister" },
	{ 0x963a020b, "trace_event_printf" },
	{ 0xbd03ed67, "this_cpu_off" },
	{ 0x95e61ae8, "__pci_register_driver" },
	{ 0xfdc4aa11, "dma_pool_create_node" },
	{ 0xd4a4112c, "device_initialize" },
	{ 0xd5bdcc8e, "debugfs_create_blob" },
	{ 0x4974b3b2, "trace_event_raw_init" },
	{ 0x2c543e67, "trace_print_symbols_seq" },
	{ 0xa53f4e29, "memcpy" },
	{ 0x01503ab7, "component_del" },
	{ 0x8e142c2e, "kstrtouint" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0xd5bc7086, "seq_lseek" },
	{ 0x3239fbdb, "x86_apple_machine" },
	{ 0x9b5f0997, "acpi_find_child_by_adr" },
	{ 0x80b75ca9, "device_for_each_child" },
	{ 0x0feb1e94, "usleep_range_state" },
	{ 0x0db8d68d, "prepare_to_wait_event" },
	{ 0x16ab4215, "__wake_up" },
	{ 0x02e1dca7, "free_pages" },
	{ 0xb63f08fb, "pci_irq_vector" },
	{ 0x184529f1, "get_device" },
	{ 0xd272d446, "pci_lock_rescan_remove" },
	{ 0xe1e1f979, "_raw_spin_lock_irqsave" },
	{ 0xd65731f8, "__dynamic_dev_dbg" },
	{ 0xde338d9a, "_raw_spin_lock" },
	{ 0x177eea2f, "pci_unregister_driver" },
	{ 0xd272d446, "__fentry__" },
	{ 0xdd6830c7, "sysfs_emit" },
	{ 0x3357ab99, "hex2bin" },
	{ 0x0f11ae8e, "trace_seq_puts" },
	{ 0xe1a85ad3, "dma_pool_alloc" },
	{ 0x2920b270, "dev_driver_string" },
	{ 0x9c0c38e8, "trace_event_buffer_commit" },
	{ 0xd0632e1b, "crypto_destroy_tfm" },
	{ 0x59fa1122, "pci_read_config_dword" },
	{ 0x5a844b26, "__x86_indirect_thunk_rax" },
	{ 0xaf2748ac, "device_match_fwnode" },
	{ 0x68680d64, "__pm_runtime_set_status" },
	{ 0xbcba8b92, "trace_seq_putc" },
	{ 0xde338d9a, "_raw_spin_lock_irq" },
	{ 0x1cd11e9e, "pm_wakeup_dev_event" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0xcc7e7f6a, "register_acpi_bus_type" },
	{ 0xd272d446, "schedule" },
	{ 0xfad2b12c, "acpi_find_child_device" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x2520ea93, "refcount_warn_saturate" },
	{ 0x8ce83585, "queue_delayed_work_on" },
	{ 0x1671ae38, "debugfs_lookup_and_remove" },
	{ 0xd4a4112c, "put_device" },
	{ 0xaacf3b28, "devm_free_irq" },
	{ 0xaf270aa8, "pm_runtime_enable" },
	{ 0x80b75ca9, "device_for_each_child_reverse" },
	{ 0xdbf85429, "pci_release_resource" },
	{ 0x9479a1e8, "strnlen" },
	{ 0x5a844b26, "__x86_indirect_thunk_rdx" },
	{ 0xce6f9b61, "bus_for_each_dev" },
	{ 0xcac6bbbd, "_dev_info" },
	{ 0x8cea15a0, "pcie_bus_configure_settings" },
	{ 0x38ac03d5, "pci_assign_unassigned_bridge_resources" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0xe118f9e0, "sysfs_create_link" },
	{ 0x48fd0ca0, "get_zeroed_page_noprof" },
	{ 0x46c12dd3, "kstrndup" },
	{ 0x43a60d1a, "kobject_uevent_env" },
	{ 0x7a5ffe84, "init_wait_entry" },
	{ 0xce40870d, "acpi_evaluate_dsm" },
	{ 0x98aacd62, "perf_trace_buf_alloc" },
	{ 0x2dcd2c88, "perf_trace_run_bpf_submit" },
	{ 0xb80cb78a, "bus_unregister" },
	{ 0xcac6bbbd, "_dev_err" },
	{ 0x5e23fb1b, "device_wakeup_enable" },
	{ 0xdccd4406, "crypto_shash_setkey" },
	{ 0xdeea409d, "bus_find_device" },
	{ 0x96845d77, "device_add" },
	{ 0x471ae204, "sysfs_remove_link" },
	{ 0xd09b06f5, "kstrtoint" },
	{ 0x28a632dc, "device_property_read_u8_array" },
	{ 0x9126ce86, "request_threaded_irq" },
	{ 0x3e1af5f1, "dma_pool_free" },
	{ 0x5a844b26, "__x86_indirect_thunk_r14" },
	{ 0x2f670524, "bpf_trace_run5" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xd7a59a65, "vmalloc_noprof" },
	{ 0x5e44572b, "pci_bus_add_devices" },
	{ 0xbeb1d261, "destroy_workqueue" },
	{ 0xf46d5bf3, "mutex_lock" },
	{ 0xa7e91503, "pci_device_is_present" },
	{ 0x4f27bb89, "dma_alloc_attrs" },
	{ 0x74fc8c8a, "debugfs_remove" },
	{ 0xd2180e38, "pci_read_config_word" },
	{ 0xde338d9a, "_raw_spin_unlock_irq" },
	{ 0x6c94137e, "seq_putc" },
	{ 0xd94efd11, "const_current_task" },
	{ 0x69b21f31, "uuid_null" },
	{ 0x8278bce6, "trace_event_reg" },
	{ 0x20550fb7, "crypto_shash_digest" },
	{ 0x4c3d335e, "ida_free" },
	{ 0x6b62c318, "driver_unregister" },
	{ 0x8ce83585, "mod_delayed_work_on" },
	{ 0xb5c51982, "__cpu_online_mask" },
	{ 0x402db74e, "memcmp" },
	{ 0x173ec8da, "sscanf" },
	{ 0x57fa0a02, "device_find_child" },
	{ 0xc1e6c71e, "__mutex_init" },
	{ 0xb5d584c5, "pcim_iomap_region" },
	{ 0x357aaab3, "mutex_lock_interruptible" },
	{ 0xe54e0a6b, "__fortify_panic" },
	{ 0x81a1a811, "_raw_spin_unlock_irqrestore" },
	{ 0x96845d77, "device_register" },
	{ 0xd4a4112c, "device_del" },
	{ 0x5373d78a, "kstrtobool" },
	{ 0x85acaba2, "cancel_delayed_work" },
	{ 0xcac6bbbd, "_dev_warn" },
	{ 0xcc7e7f6a, "unregister_acpi_bus_type" },
	{ 0x5a844b26, "__x86_indirect_thunk_r10" },
	{ 0xb6e17fe6, "acpi_get_first_physical_node" },
	{ 0xc72bd3ae, "pci_set_master" },
	{ 0xbeb1d261, "__flush_workqueue" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x386e4ba3, "kmemdup_noprof" },
	{ 0xfe056791, "uuid_parse" },
	{ 0xaf270aa8, "pm_runtime_no_callbacks" },
	{ 0xec203997, "kasprintf" },
	{ 0x888b8f57, "strcmp" },
	{ 0x357aaab3, "mutex_trylock" },
	{ 0xbd03ed67, "USER_PTR_MAX" },
	{ 0x058c185a, "jiffies" },
	{ 0xf296206e, "pm_suspend_global_flags" },
	{ 0x04dde6dd, "dma_set_coherent_mask" },
	{ 0xce4af33b, "kstrdup" },
	{ 0x11bacf83, "seq_read" },
	{ 0x3239fbdb, "osc_sb_native_usb4_support_confirmed" },
	{ 0xdd6830c7, "sprintf" },
	{ 0x3cfa7c1b, "fwnode_find_reference" },
	{ 0x8bc8f40f, "crc32c" },
	{ 0x82fd7238, "__ubsan_handle_shift_out_of_bounds" },
	{ 0x7ec472ba, "cpu_number" },
	{ 0xab48d875, "dma_pool_destroy" },
	{ 0x7ec472ba, "__preempt_count" },
	{ 0xaf270aa8, "pm_runtime_forbid" },
	{ 0xd10b475e, "dma_free_attrs" },
	{ 0xf1de9e85, "vfree" },
	{ 0xaf270aa8, "pm_runtime_allow" },
	{ 0x842d17bc, "trace_event_buffer_reserve" },
	{ 0x8a07be69, "__pm_runtime_resume" },
	{ 0xa5c7582d, "strsep" },
	{ 0xf46d5bf3, "mutex_unlock" },
	{ 0x85acaba2, "cancel_delayed_work_sync" },
	{ 0xd272d446, "pci_unlock_rescan_remove" },
	{ 0x5d25d677, "acpi_fetch_acpi_dev" },
	{ 0x4d8419c6, "param_ops_bool" },
	{ 0xe459c92c, "osc_sb_native_usb4_control" },
	{ 0xc1a391ea, "pci_dev_get" },
	{ 0xd45ca0d6, "seq_write" },
	{ 0xecd17989, "__kmalloc_cache_noprof" },
	{ 0x97acb853, "ktime_get" },
	{ 0x546c19d9, "validate_usercopy_range" },
	{ 0x75738bed, "__warn_printk" },
	{ 0x0d8b6c91, "seq_printf" },
	{ 0xc01aafd2, "get_random_u32" },
	{ 0x71798f7e, "delayed_work_timer_fn" },
	{ 0x6e1f7feb, "kobject_uevent" },
	{ 0x6d2a89ad, "debugfs_create_file_full" },
	{ 0xe64b1d36, "device_iommu_capable" },
	{ 0x024d45a2, "single_release" },
	{ 0x02f9bbf0, "timer_init_key" },
	{ 0x04dde6dd, "dma_set_mask" },
	{ 0x1b5015ce, "pci_stop_and_remove_bus_device" },
	{ 0x224a53e7, "get_random_bytes" },
	{ 0x265b047e, "pci_walk_bus" },
	{ 0x13a0c3af, "dev_err_probe" },
	{ 0xd79b18fb, "acpi_check_dsm" },
	{ 0x636e75e0, "nvmem_register" },
	{ 0x8a07be69, "__pm_runtime_suspend" },
	{ 0xa2035824, "component_add" },
	{ 0xdf4bee3d, "alloc_workqueue_noprof" },
	{ 0xe4de56b4, "__ubsan_handle_load_invalid_value" },
	{ 0x43a349ca, "strlen" },
	{ 0x375138a5, "__pm_runtime_use_autosuspend" },
	{ 0x4d8419c6, "param_ops_int" },
	{ 0xd272d446, "__SCT__preempt_schedule_notrace" },
	{ 0xce105414, "single_open" },
	{ 0xfa4b3a58, "pcim_enable_device" },
	{ 0x4ead105d, "__pm_runtime_disable" },
	{ 0x0e435208, "crypto_alloc_shash" },
	{ 0x2379af1d, "debugfs_create_dir" },
	{ 0xa8867e22, "pci_write_config_word" },
	{ 0xde338d9a, "_raw_spin_unlock" },
	{ 0x251c400a, "device_link_add" },
	{ 0x955467e2, "trace_handle_return" },
	{ 0xb417b766, "fwnode_property_read_u8_array" },
	{ 0x5a844b26, "__x86_indirect_thunk_r8" },
	{ 0xc95d21ab, "driver_register" },
	{ 0x67b2ba98, "sysfs_emit_at" },
	{ 0x8a07be69, "__pm_runtime_idle" },
	{ 0x67628f51, "msleep" },
	{ 0x7851be11, "__SCT__might_resched" },
	{ 0x1aab75ab, "__dev_fwnode" },
	{ 0xebef3db0, "pci_bus_type" },
	{ 0x2b66fd0e, "pci_write_config_dword" },
	{ 0x08bfc903, "kmalloc_caches" },
	{ 0x5b7ed85d, "bus_register" },
	{ 0xaef1f20d, "system_wq" },
	{ 0x2d88a3ab, "flush_work" },
	{ 0x814e12e5, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x21f931bb,
	0x9dd4105e,
	0xd98156e2,
	0x827878ff,
	0x7e2232fb,
	0xa61fd7aa,
	0x95305a11,
	0x63f4e388,
	0x52b27c53,
	0x0c60341d,
	0x97195299,
	0x740648c4,
	0xba0a7756,
	0x4d8419c6,
	0x092a35a2,
	0x57860fb4,
	0x12f6a6e7,
	0xfad8f384,
	0x42e82979,
	0x534ed5f3,
	0xd710adbf,
	0x41495f0d,
	0x143526e8,
	0xa6c3b74d,
	0x64ed6ec2,
	0x0845aed0,
	0x40a621c5,
	0x65026e43,
	0x49733ad6,
	0xabdb103e,
	0x7a4352ad,
	0xd4a4112c,
	0x6363ea20,
	0x60c9c0b3,
	0xfccc5035,
	0xc87f4bab,
	0x5913fe24,
	0x963a020b,
	0xbd03ed67,
	0x95e61ae8,
	0xfdc4aa11,
	0xd4a4112c,
	0xd5bdcc8e,
	0x4974b3b2,
	0x2c543e67,
	0xa53f4e29,
	0x01503ab7,
	0x8e142c2e,
	0xcb8b6ec6,
	0xd5bc7086,
	0x3239fbdb,
	0x9b5f0997,
	0x80b75ca9,
	0x0feb1e94,
	0x0db8d68d,
	0x16ab4215,
	0x02e1dca7,
	0xb63f08fb,
	0x184529f1,
	0xd272d446,
	0xe1e1f979,
	0xd65731f8,
	0xde338d9a,
	0x177eea2f,
	0xd272d446,
	0xdd6830c7,
	0x3357ab99,
	0x0f11ae8e,
	0xe1a85ad3,
	0x2920b270,
	0x9c0c38e8,
	0xd0632e1b,
	0x59fa1122,
	0x5a844b26,
	0xaf2748ac,
	0x68680d64,
	0xbcba8b92,
	0xde338d9a,
	0x1cd11e9e,
	0xbd03ed67,
	0xcc7e7f6a,
	0xd272d446,
	0xfad2b12c,
	0xd272d446,
	0x2520ea93,
	0x8ce83585,
	0x1671ae38,
	0xd4a4112c,
	0xaacf3b28,
	0xaf270aa8,
	0x80b75ca9,
	0xdbf85429,
	0x9479a1e8,
	0x5a844b26,
	0xce6f9b61,
	0xcac6bbbd,
	0x8cea15a0,
	0x38ac03d5,
	0x90a48d82,
	0xe118f9e0,
	0x48fd0ca0,
	0x46c12dd3,
	0x43a60d1a,
	0x7a5ffe84,
	0xce40870d,
	0x98aacd62,
	0x2dcd2c88,
	0xb80cb78a,
	0xcac6bbbd,
	0x5e23fb1b,
	0xdccd4406,
	0xdeea409d,
	0x96845d77,
	0x471ae204,
	0xd09b06f5,
	0x28a632dc,
	0x9126ce86,
	0x3e1af5f1,
	0x5a844b26,
	0x2f670524,
	0xbd03ed67,
	0xd7a59a65,
	0x5e44572b,
	0xbeb1d261,
	0xf46d5bf3,
	0xa7e91503,
	0x4f27bb89,
	0x74fc8c8a,
	0xd2180e38,
	0xde338d9a,
	0x6c94137e,
	0xd94efd11,
	0x69b21f31,
	0x8278bce6,
	0x20550fb7,
	0x4c3d335e,
	0x6b62c318,
	0x8ce83585,
	0xb5c51982,
	0x402db74e,
	0x173ec8da,
	0x57fa0a02,
	0xc1e6c71e,
	0xb5d584c5,
	0x357aaab3,
	0xe54e0a6b,
	0x81a1a811,
	0x96845d77,
	0xd4a4112c,
	0x5373d78a,
	0x85acaba2,
	0xcac6bbbd,
	0xcc7e7f6a,
	0x5a844b26,
	0xb6e17fe6,
	0xc72bd3ae,
	0xbeb1d261,
	0xd272d446,
	0x386e4ba3,
	0xfe056791,
	0xaf270aa8,
	0xec203997,
	0x888b8f57,
	0x357aaab3,
	0xbd03ed67,
	0x058c185a,
	0xf296206e,
	0x04dde6dd,
	0xce4af33b,
	0x11bacf83,
	0x3239fbdb,
	0xdd6830c7,
	0x3cfa7c1b,
	0x8bc8f40f,
	0x82fd7238,
	0x7ec472ba,
	0xab48d875,
	0x7ec472ba,
	0xaf270aa8,
	0xd10b475e,
	0xf1de9e85,
	0xaf270aa8,
	0x842d17bc,
	0x8a07be69,
	0xa5c7582d,
	0xf46d5bf3,
	0x85acaba2,
	0xd272d446,
	0x5d25d677,
	0x4d8419c6,
	0xe459c92c,
	0xc1a391ea,
	0xd45ca0d6,
	0xecd17989,
	0x97acb853,
	0x546c19d9,
	0x75738bed,
	0x0d8b6c91,
	0xc01aafd2,
	0x71798f7e,
	0x6e1f7feb,
	0x6d2a89ad,
	0xe64b1d36,
	0x024d45a2,
	0x02f9bbf0,
	0x04dde6dd,
	0x1b5015ce,
	0x224a53e7,
	0x265b047e,
	0x13a0c3af,
	0xd79b18fb,
	0x636e75e0,
	0x8a07be69,
	0xa2035824,
	0xdf4bee3d,
	0xe4de56b4,
	0x43a349ca,
	0x375138a5,
	0x4d8419c6,
	0xd272d446,
	0xce105414,
	0xfa4b3a58,
	0x4ead105d,
	0x0e435208,
	0x2379af1d,
	0xa8867e22,
	0xde338d9a,
	0x251c400a,
	0x955467e2,
	0xb417b766,
	0x5a844b26,
	0xc95d21ab,
	0x67b2ba98,
	0x8a07be69,
	0x67628f51,
	0x7851be11,
	0x1aab75ab,
	0xebef3db0,
	0x2b66fd0e,
	0x08bfc903,
	0x5b7ed85d,
	0xaef1f20d,
	0x2d88a3ab,
	0x814e12e5,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"add_uevent_var\0"
	"free_irq\0"
	"ida_alloc_range\0"
	"pcie_capability_read_word\0"
	"ioread32\0"
	"__check_object_size\0"
	"pci_scan_child_bus\0"
	"pci_find_ext_capability\0"
	"is_acpi_device_node\0"
	"device_set_wakeup_capable\0"
	"bpf_trace_run4\0"
	"ida_destroy\0"
	"devm_request_threaded_irq\0"
	"param_ops_uint\0"
	"_copy_from_user\0"
	"wait_for_completion_timeout\0"
	"devm_kmalloc\0"
	"iowrite32\0"
	"acpi_walk_namespace\0"
	"__msecs_to_jiffies\0"
	"__kmalloc_noprof\0"
	"strim\0"
	"dev_set_name\0"
	"ktime_get_mono_fast_ns\0"
	"trace_seq_printf\0"
	"pci_alloc_irq_vectors\0"
	"snprintf\0"
	"complete\0"
	"queue_work_on\0"
	"trace_raw_output_prep\0"
	"pci_dev_put\0"
	"device_unregister\0"
	"pm_runtime_set_autosuspend_delay\0"
	"__init_swait_queue_head\0"
	"__trace_trigger_soft_disabled\0"
	"finish_wait\0"
	"nvmem_unregister\0"
	"trace_event_printf\0"
	"this_cpu_off\0"
	"__pci_register_driver\0"
	"dma_pool_create_node\0"
	"device_initialize\0"
	"debugfs_create_blob\0"
	"trace_event_raw_init\0"
	"trace_print_symbols_seq\0"
	"memcpy\0"
	"component_del\0"
	"kstrtouint\0"
	"kfree\0"
	"seq_lseek\0"
	"x86_apple_machine\0"
	"acpi_find_child_by_adr\0"
	"device_for_each_child\0"
	"usleep_range_state\0"
	"prepare_to_wait_event\0"
	"__wake_up\0"
	"free_pages\0"
	"pci_irq_vector\0"
	"get_device\0"
	"pci_lock_rescan_remove\0"
	"_raw_spin_lock_irqsave\0"
	"__dynamic_dev_dbg\0"
	"_raw_spin_lock\0"
	"pci_unregister_driver\0"
	"__fentry__\0"
	"sysfs_emit\0"
	"hex2bin\0"
	"trace_seq_puts\0"
	"dma_pool_alloc\0"
	"dev_driver_string\0"
	"trace_event_buffer_commit\0"
	"crypto_destroy_tfm\0"
	"pci_read_config_dword\0"
	"__x86_indirect_thunk_rax\0"
	"device_match_fwnode\0"
	"__pm_runtime_set_status\0"
	"trace_seq_putc\0"
	"_raw_spin_lock_irq\0"
	"pm_wakeup_dev_event\0"
	"__ref_stack_chk_guard\0"
	"register_acpi_bus_type\0"
	"schedule\0"
	"acpi_find_child_device\0"
	"__stack_chk_fail\0"
	"refcount_warn_saturate\0"
	"queue_delayed_work_on\0"
	"debugfs_lookup_and_remove\0"
	"put_device\0"
	"devm_free_irq\0"
	"pm_runtime_enable\0"
	"device_for_each_child_reverse\0"
	"pci_release_resource\0"
	"strnlen\0"
	"__x86_indirect_thunk_rdx\0"
	"bus_for_each_dev\0"
	"_dev_info\0"
	"pcie_bus_configure_settings\0"
	"pci_assign_unassigned_bridge_resources\0"
	"__ubsan_handle_out_of_bounds\0"
	"sysfs_create_link\0"
	"get_zeroed_page_noprof\0"
	"kstrndup\0"
	"kobject_uevent_env\0"
	"init_wait_entry\0"
	"acpi_evaluate_dsm\0"
	"perf_trace_buf_alloc\0"
	"perf_trace_run_bpf_submit\0"
	"bus_unregister\0"
	"_dev_err\0"
	"device_wakeup_enable\0"
	"crypto_shash_setkey\0"
	"bus_find_device\0"
	"device_add\0"
	"sysfs_remove_link\0"
	"kstrtoint\0"
	"device_property_read_u8_array\0"
	"request_threaded_irq\0"
	"dma_pool_free\0"
	"__x86_indirect_thunk_r14\0"
	"bpf_trace_run5\0"
	"random_kmalloc_seed\0"
	"vmalloc_noprof\0"
	"pci_bus_add_devices\0"
	"destroy_workqueue\0"
	"mutex_lock\0"
	"pci_device_is_present\0"
	"dma_alloc_attrs\0"
	"debugfs_remove\0"
	"pci_read_config_word\0"
	"_raw_spin_unlock_irq\0"
	"seq_putc\0"
	"const_current_task\0"
	"uuid_null\0"
	"trace_event_reg\0"
	"crypto_shash_digest\0"
	"ida_free\0"
	"driver_unregister\0"
	"mod_delayed_work_on\0"
	"__cpu_online_mask\0"
	"memcmp\0"
	"sscanf\0"
	"device_find_child\0"
	"__mutex_init\0"
	"pcim_iomap_region\0"
	"mutex_lock_interruptible\0"
	"__fortify_panic\0"
	"_raw_spin_unlock_irqrestore\0"
	"device_register\0"
	"device_del\0"
	"kstrtobool\0"
	"cancel_delayed_work\0"
	"_dev_warn\0"
	"unregister_acpi_bus_type\0"
	"__x86_indirect_thunk_r10\0"
	"acpi_get_first_physical_node\0"
	"pci_set_master\0"
	"__flush_workqueue\0"
	"__x86_return_thunk\0"
	"kmemdup_noprof\0"
	"uuid_parse\0"
	"pm_runtime_no_callbacks\0"
	"kasprintf\0"
	"strcmp\0"
	"mutex_trylock\0"
	"USER_PTR_MAX\0"
	"jiffies\0"
	"pm_suspend_global_flags\0"
	"dma_set_coherent_mask\0"
	"kstrdup\0"
	"seq_read\0"
	"osc_sb_native_usb4_support_confirmed\0"
	"sprintf\0"
	"fwnode_find_reference\0"
	"crc32c\0"
	"__ubsan_handle_shift_out_of_bounds\0"
	"cpu_number\0"
	"dma_pool_destroy\0"
	"__preempt_count\0"
	"pm_runtime_forbid\0"
	"dma_free_attrs\0"
	"vfree\0"
	"pm_runtime_allow\0"
	"trace_event_buffer_reserve\0"
	"__pm_runtime_resume\0"
	"strsep\0"
	"mutex_unlock\0"
	"cancel_delayed_work_sync\0"
	"pci_unlock_rescan_remove\0"
	"acpi_fetch_acpi_dev\0"
	"param_ops_bool\0"
	"osc_sb_native_usb4_control\0"
	"pci_dev_get\0"
	"seq_write\0"
	"__kmalloc_cache_noprof\0"
	"ktime_get\0"
	"validate_usercopy_range\0"
	"__warn_printk\0"
	"seq_printf\0"
	"get_random_u32\0"
	"delayed_work_timer_fn\0"
	"kobject_uevent\0"
	"debugfs_create_file_full\0"
	"device_iommu_capable\0"
	"single_release\0"
	"timer_init_key\0"
	"dma_set_mask\0"
	"pci_stop_and_remove_bus_device\0"
	"get_random_bytes\0"
	"pci_walk_bus\0"
	"dev_err_probe\0"
	"acpi_check_dsm\0"
	"nvmem_register\0"
	"__pm_runtime_suspend\0"
	"component_add\0"
	"alloc_workqueue_noprof\0"
	"__ubsan_handle_load_invalid_value\0"
	"strlen\0"
	"__pm_runtime_use_autosuspend\0"
	"param_ops_int\0"
	"__SCT__preempt_schedule_notrace\0"
	"single_open\0"
	"pcim_enable_device\0"
	"__pm_runtime_disable\0"
	"crypto_alloc_shash\0"
	"debugfs_create_dir\0"
	"pci_write_config_word\0"
	"_raw_spin_unlock\0"
	"device_link_add\0"
	"trace_handle_return\0"
	"fwnode_property_read_u8_array\0"
	"__x86_indirect_thunk_r8\0"
	"driver_register\0"
	"sysfs_emit_at\0"
	"__pm_runtime_idle\0"
	"msleep\0"
	"__SCT__might_resched\0"
	"__dev_fwnode\0"
	"pci_bus_type\0"
	"pci_write_config_dword\0"
	"kmalloc_caches\0"
	"bus_register\0"
	"system_wq\0"
	"flush_work\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");

MODULE_ALIAS("pci:v00008086d00001513sv00002222sd00001111bc08sc80i00*");
MODULE_ALIAS("pci:v00008086d00001547sv00002222sd00001111bc08sc80i00*");
MODULE_ALIAS("pci:v00008086d0000156Asv*sd*bc08sc80i00*");
MODULE_ALIAS("pci:v00008086d0000156Csv*sd*bc08sc80i00*");
MODULE_ALIAS("pci:v00008086d00001575sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001577sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000015DDsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000015BFsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000015DCsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000015D9sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000015D2sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000015DEsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000015E8sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000015EBsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00008A17sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00008A0Dsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00009A1Bsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00009A1Dsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00009A1Fsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00009A21sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000463Esv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000466Dsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000A73Esv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000A76Dsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00007EB2sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00007EC2sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00007EC3sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000A833sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000A834sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000E333sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000E334sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000E433sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000E434sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00004D33sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00005781sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00005784sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v*d*sv*sd*bc0Csc03i40*");

MODULE_INFO(srcversion, "C64E66F03AD02267BE28123");
