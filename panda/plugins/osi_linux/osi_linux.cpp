/*!
 * @file osi_linux.cpp
 * @brief PANDA Operating System Introspection for Linux.
 *
 * @author Manolis Stamatogiannakis <manolis.stamatogiannakis@vu.nl>
 * @copyright This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 */
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <map>
#include <glib.h>

#include "panda/plugin.h"
#include "panda/plugin_plugin.h"
#include "osi/osi_types.h"
#include "osi/os_intro.h"
#include "utils/kernelinfo/kernelinfo.h"
#include "osi_linux.h"

#include "default_profile.h"
#include "kernel_2_4_x_profile.h"

/*
 * Functions interfacing with QEMU/PANDA should be linked as C.
 * C++ function name mangling breaks linkage.
 */
extern "C" {
bool init_plugin(void *);
void uninit_plugin(void *);
#include "osi_linux_int_fns.h"
}

void on_get_processes(CPUState *env, GArray **out);
void on_get_process_handles(CPUState *env, GArray **out);
void on_get_current_process(CPUState *env, OsiProc **out_p);
void on_get_process(CPUState *, const OsiProcHandle *, OsiProc **);
void on_get_libraries(CPUState *env, OsiProc *p, GArray **out);
void on_get_current_thread(CPUState *env, OsiThread *t);

struct kernelinfo ki;
struct KernelProfile const *kernel_profile = &DEFAULT_PROFILE;

/* ******************************************************************
 Helpers
****************************************************************** */

/**
 * @brief Resolves a file struct and returns its full pathname.
 */
static char *get_file_name(CPUState *env, target_ptr_t file_struct) {
	char *name = NULL;
	target_ptr_t file_dentry, file_mnt;

	// Read addresses for dentry, vfsmnt structs.
	file_dentry = get_file_dentry(env, file_struct);
	file_mnt = get_file_mnt(env, file_struct);

	if (unlikely(file_dentry == (target_ptr_t)NULL || file_mnt == (target_ptr_t)NULL)) {
		LOG_INFO("failure resolving file struct " TARGET_PTR_FMT "/" TARGET_PTR_FMT, file_dentry, file_mnt);
		return NULL;
	}

	char *s1, *s2;
	s1 = read_vfsmount_name(env, file_mnt);
	s2 = read_dentry_name(env, file_dentry);
	name = g_strconcat(s1, s2, NULL);
	g_free(s1);
	g_free(s2);

	return name;
}

static uint64_t get_file_position(CPUState *env, target_ptr_t file_struct) {
	return get_file_pos(env, file_struct);
}

static target_ptr_t get_file_struct_ptr(CPUState *env, target_ptr_t task_struct, int fd) {
	target_ptr_t files = get_files(env, task_struct);
	target_ptr_t fds = kernel_profile->get_files_fds(env, files);
	target_ptr_t fd_file_ptr, fd_file;

	// fds is a flat array with struct file pointers.
	// Calculate the address of the nth pointer and read it.
	fd_file_ptr = fds + fd*sizeof(target_ptr_t);
	if (-1 == panda_virtual_memory_rw(env, fd_file_ptr, (uint8_t *)&fd_file, sizeof(target_ptr_t), 0)) {
		return (target_ptr_t)NULL;
	}
	if (fd_file == (target_ptr_t)NULL) {
		return (target_ptr_t)NULL;
	}
	return fd_file;
}

/**
 * @brief Resolves a file struct and returns its full pathname.
 */
static char *get_fd_name(CPUState *env, target_ptr_t task_struct, int fd) {
	target_ptr_t fd_file = get_file_struct_ptr(env, task_struct, fd);
	if (fd_file == (target_ptr_t)NULL) return NULL;
	return get_file_name(env, fd_file);
}

/**
 * @brief Retrieves the current offset of a file descriptor.
 */
static uint64_t get_fd_pos(CPUState *env, target_ptr_t task_struct, int fd) {
	target_ptr_t fd_file = get_file_struct_ptr(env, task_struct, fd);
	if (fd_file == (target_ptr_t)NULL) return ((uint64_t) INVALID_FILE_POS);
	return get_file_position(env, fd_file);
}

/**
 * @brief Template function for extracting data for all running processes.
 * This can be used to quickly implement extraction of partial process
 * information without having to rewrite the process list traversal
 * code.
 *
 * @note The ascii pictogram in kernel_structs.html roughly explains how the
 * process list traversal works. However, it may be inacurrate for some corner
 * cases. E.g. it doesn't explain why some inifnite loop cases manifest.
 * Avoiding these infinite loops was mostly a trial+error process.
 */
template <typename ET>
void get_process_info(CPUState *env, GArray **out,
					  void (*fill_element)(CPUState *, ET *, target_ptr_t),
					  void (*free_element_contents)(ET *)) {
	ET element;
	target_ptr_t ts_first, ts_current;
	target_ptr_t UNUSED(tg_first), UNUSED(tg_next);

	if (*out == NULL) {
		// g_array_sized_new() args: zero_term, clear, element_sz, reserved_sz
		*out = g_array_sized_new(false, false, sizeof(ET), 128);
		g_array_set_clear_func(*out, (GDestroyNotify)free_element_contents);
	}

#if defined(OSI_LINUX_LIST_FROM_INIT)
	// Start process enumeration from the init task.
	ts_first = ki.task.init_addr;
#else
	// Start process enumeration (roughly) from the current task. This is the default.
	ts_first = kernel_profile->get_current_task_struct(env);

	// To avoid infinite loops, we need to actually start traversal from the next
	// process after the thread group leader of the current task.
	ts_first = kernel_profile->get_group_leader(env, ts_first);
	ts_first = kernel_profile->get_task_struct_next(env, ts_first);
#endif

	ts_current = ts_first;
	if (ts_first == (target_ptr_t)NULL) goto error;
#if defined(OSI_LINUX_PSDEBUG)
	LOG_INFO("START %c:%c " TARGET_PTR_FMT " " TARGET_PTR_FMT, TS_THREAD_CHR(env, ts_first),  TS_LEADER_CHR(env, ts_first), ts_first, ts_first);
#endif

	do {
#if defined(OSI_LINUX_PSDEBUG)
		LOG_INFO("\t %03u:" TARGET_PTR_FMT ":" TARGET_PID_FMT ":" TARGET_PID_FMT ":%c:%c", a->len, ts_current, get_pid(env, ts_current), get_tgid(env, ts_current), TS_THREAD_CHR(env, ts_current), TS_LEADER_CHR(env, ts_current));
#endif
		memset(&element, 0, sizeof(ET));
		fill_element(env, &element, ts_current);
		g_array_append_val(*out, element);
		OSI_MAX_PROC_CHECK((*out)->len, "traversing process list");

#if defined(OSI_LINUX_LIST_THREADS)
		// Traverse thread group list.
		// It is assumed that ts_current is a thread group leader.
		tg_first = ts_current + ki.task.thread_group_offset;
		while ((tg_next = get_thread_group(env, ts_current)) != tg_first) {
			ts_current = tg_next - ki.task.thread_group_offset;
#if defined(OSI_LINUX_PSDEBUG)
			LOG_INFO("\t %03u:" TARGET_PTR_FMT ":" TARGET_PID_FMT ":" TARGET_PID_FMT ":%c:%c", a->len, ts_current, get_pid(env, ts_current), get_tgid(env, ts_current), TS_THREAD_CHR(env, ts_current), TS_LEADER_CHR(env, ts_current));
#endif
			memset(&element, 0, sizeof(ET));
			element_fill(env, &element, ts_current);
			g_array_append_val(*out, element);
			OSI_MAX_PROC_CHECK((*out)->len, "traversing thread group list");
		}
		ts_current = tg_first - ki.task.thread_group_offset;
#endif

		ts_current = kernel_profile->get_task_struct_next(env, ts_current);
	} while(ts_current != (target_ptr_t)NULL && ts_current != ts_first);

	// memory read error
	if (ts_current == (target_ptr_t)NULL) goto error;

	return;

error:
	g_array_free(*out, true);  // safe even when *out == NULL
	*out = NULL;
	return;
}
/**
 * @brief Fills an OsiProcHandle struct.
 */
static void fill_osiprochandle(CPUState *env, OsiProcHandle *h,
						   target_ptr_t task_addr) {
	h->taskd = task_addr;
	h->asid = panda_virt_to_phys(env, get_pgd(env, task_addr));
}

/**
 * @brief Fills an OsiProc struct. Any existing contents are overwritten.
 */
void fill_osiproc(CPUState *env, OsiProc *p, target_ptr_t task_addr) {
	memset(p, 0, sizeof(OsiProc));

	p->taskd = task_addr;
	p->name = get_name(env, task_addr, p->name);
	p->pid = get_tgid(env, task_addr);
	p->ppid = get_real_parent_pid(env, task_addr);
	p->pages = NULL;  // OsiPage - TODO

	// task_struct contains the virtual address of the pgd
	// Convert it to physycal, so it can be directly matched with the value
	// of the pgd register.
	p->asid = panda_virt_to_phys(env, get_pgd(env, task_addr));
}

/**
 * @brief Fills an OsiModule struct.
 */
static void fill_osimodule(CPUState *env, OsiModule *m, target_ptr_t vma_addr) {
	target_ulong vma_start, vma_end;
	target_ptr_t vma_vm_file;
	target_ptr_t vma_dentry;
	target_ptr_t mm_addr, start_brk, brk, start_stack;

	vma_start = get_vma_start(env, vma_addr);
	vma_end = get_vma_end(env, vma_addr);
	vma_vm_file = get_vma_vm_file(env, vma_addr);

	// Fill everything but m->name and m->file.
	m->modd = vma_addr;
	m->base = vma_start;
	m->size = vma_end - vma_start;

	if (vma_vm_file !=
		(target_ptr_t)NULL) {  // Memory area is mapped from a file.
		vma_dentry = get_vma_dentry(env, vma_addr);
		m->file = read_dentry_name(env, vma_dentry);
		m->name = g_strrstr(m->file, "/");
		if (m->name != NULL) m->name = g_strdup(m->name + 1);
	} else {  // Other memory areas.
		mm_addr = get_vma_vm_mm(env, vma_addr);
		start_brk = get_mm_start_brk(env, mm_addr);
		brk = get_mm_brk(env, mm_addr);
		start_stack = get_mm_start_stack(env, mm_addr);

		m->file = NULL;
		if (vma_start <= start_brk && vma_end >= brk) {
			m->name = g_strdup("[heap]");
		} else if (vma_start <= start_stack && vma_end >= start_stack) {
			m->name = g_strdup("[stack]");
		} else {
			m->name = g_strdup("[???]");
		}
	}
}

/**
 * @brief Fills an OsiThread struct. Any existing contents are overwritten.
 */
void fill_osithread(CPUState *env, OsiThread *t,
						   target_ptr_t task_addr) {
	memset(t, 0, sizeof(*t));
	t->tid = get_pid(env, task_addr);
	t->pid = get_tgid(env, task_addr);
}

/* ******************************************************************
 PPP Callbacks
****************************************************************** */

/**
 * @brief PPP callback to retrieve process list from the running OS.
 *
 */
void on_get_processes(CPUState *env, GArray **out) {
	// instantiate and call function from get_process_info template
	get_process_info<>(env, out, fill_osiproc, free_osiproc_contents);
}

/**
 * @brief PPP callback to retrieve process handles from the running OS.
 */
void on_get_process_handles(CPUState *env, GArray **out) {
	// use a dummy free functioon instead of free_osiprochandle_contents()
	//decltype(free_osiprochandle_contents) *dummy_free = NULL;

	// instantiate and call function from get_process_info template
	get_process_info<>(env, out, fill_osiprochandle, free_osiprochandle_contents);
}

/**
 * @brief PPP callback to retrieve info about the currently running process.
 */
void on_get_current_process(CPUState *env, OsiProc **out) {
	OsiProc *p = NULL;
	target_ptr_t ts = kernel_profile->get_current_task_struct(env);
	if (ts) {
		p = (OsiProc *)g_malloc(sizeof(OsiProc));
		fill_osiproc(env, p, ts);
	}
	*out = p;
}

/**
 * @brief PPP callback to retrieve info about a running process using its
 * handle.
 */
void on_get_process(CPUState *env, const OsiProcHandle *h, OsiProc **out) {
	OsiProc *p = NULL;
	if (h != NULL && h->taskd != (target_ptr_t)NULL) {
		p = (OsiProc *)g_malloc(sizeof(OsiProc));
		fill_osiproc(env, p, h->taskd);
	}
	*out = p;
}

/**
 * @brief PPP callback to retrieve OsiModules from the running OS.
 *
 * Current implementation returns all the memory areas mapped by the
 * process and the files they were mapped from. Libraries that have
 * many mappings will appear multiple times.
 *
 * @todo Remove duplicates from results.
 */
void on_get_libraries(CPUState *env, OsiProc *p, GArray **out) {
	OsiModule m;
	target_ptr_t vma_first, vma_current;

	// Read the module info for the process.
	vma_first = vma_current = get_vma_first(env, p->taskd);
	if (vma_current == (target_ptr_t)NULL) goto error0;

	if (*out == NULL) {
		// g_array_sized_new() args: zero_term, clear, element_sz, reserved_sz
		*out = g_array_sized_new(false, false, sizeof(OsiModule), 128);
		g_array_set_clear_func(*out, (GDestroyNotify)free_osimodule_contents);
	}

	do {
		memset(&m, 0, sizeof(OsiModule));
		fill_osimodule(env, &m, vma_current);
		g_array_append_val(*out, m);
		vma_current = get_vma_next(env, vma_current);
	} while(vma_current != (target_ptr_t)NULL && vma_current != vma_first);

	return;

error0:
	g_array_free(*out, true);  // safe even when *out == NULL
	*out = NULL;
	return;
}

/**
 * @brief PPP callback to retrieve current thread.
 */
void on_get_current_thread(CPUState *env, OsiThread **out) {
	OsiThread *t = NULL;
	target_ptr_t ts = kernel_profile->get_current_task_struct(env);
	if (ts) {
		t = (OsiThread *)g_malloc(sizeof(OsiThread));
		fill_osithread(env, t, ts);
	}
	*out = t;
}

/* ******************************************************************
 osi_linux extra API
****************************************************************** */

char *osi_linux_fd_to_filename(CPUState *env, OsiProc *p, int fd) {
	target_ptr_t ts_current = p->taskd;
	char *filename = NULL;
	const char *err = NULL;

	if (ts_current == 0) {
		err = "can't get task";
		goto end;
	}

	filename = get_fd_name(env, ts_current, fd);
	if (unlikely(filename == NULL)) {
		err = "can't get filename";
		goto end;
	}

	filename = g_strchug(filename);
	if (unlikely(g_strcmp0(filename, "") == 0)) {
		err = "filename is empty";
		g_free(filename);
		filename = NULL;
		goto end;
	}

end:
	if (unlikely(err != NULL)) {
		LOG_ERROR("%s -- (pid=%d, fd=%d)", err, (int)p->pid, fd);
	}
	return filename;
}


unsigned long long  osi_linux_fd_to_pos(CPUState *env, OsiProc *p, int fd) {
	//	target_ulong asid = panda_current_asid(env);
	target_ptr_t ts_current = 0;
	ts_current = p->taskd;
	if (ts_current == 0) return INVALID_FILE_POS;
	return get_fd_pos(env, ts_current, fd);
}



/* ******************************************************************
 Testing functions
****************************************************************** */
#if (defined OSI_LINUX_TEST)
/**
 * @brief Tests the osi_linux functionality by directly calling the
 * respective introspection functions. For testing the functions via
 * their callbacks, use the osi_test plugin.
 */
int osi_linux_test(CPUState *env, target_ulong oldval, target_ulong newval) {
	static uint32_t asid_change_count = 0;
	char mode = panda_in_kernel(env) ? 'K' : 'U';

	LOG_INFO("--- START(%c) %06u ------------------------------------------", mode, asid_change_count);
	OsiProcs *ps = NULL;
	on_get_processes(env, &ps);
	for (uint32_t i=0; i<ps->num; i++) {
		OsiProc *p = &ps->proc[i];
		LOG_INFO(TARGET_FMT_PID ":" TARGET_FMT_PID ":%s:" TARGET_PTR_FMT ":" TARGET_PTR_FMT,
				(int)p->pid, (int)p->ppid, p->name, p->asid, p->taskd);
#if defined(OSI_LINUX_TEST_MODULES)
		OsiModules *ms = NULL;
		on_get_libraries(env, p, &ms);
		if (ms != NULL) {
			for (uint32_t j=0; j<ms->num; j++) {
				OsiModule *m = &ms->module[j];
				LOG_INFO("\t" TARGET_PTR_FMT ":%04up:%s:%s", m->base, NPAGES(m->size), m->name, m->file);
			}
			on_free_osimodules(ms);
		}
#endif
#if defined(OSI_LINUX_TEST_MODULES) && defined(OSI_LINUX_TEST_FDNAME)
		if (ms != NULL) {
			LOG_INFO("\t------------------------");
		}
#endif
#if defined(OSI_LINUX_TEST_FDNAME)
		for (uint32_t fd=0; fd<16; fd++) {
			char *s = get_fd_name(env, ps->proc[i].offset, fd);
			LOG_INFO("\tfd%d -> %s", fd, s);
			g_free(s);
		}
#endif
	}
	on_free_osiprocs(ps);
	LOG_INFO("--- END(%c)  %06u ------------------------------------------", mode, asid_change_count);
	asid_change_count++;
	return 0;
}
#endif

/* ******************************************************************
 Plugin Initialization/Cleanup
****************************************************************** */

/**
 * @brief Initializes plugin.
 */
bool init_plugin(void *self) {
#if defined(TARGET_I386) || defined(TARGET_ARM)
#if (defined OSI_LINUX_TEST)
	panda_cb pcb = { .asid_changed = osi_linux_test };
	panda_register_callback(self, PANDA_CB_ASID_CHANGED, pcb);
#endif

	// Read the name of the kernel configuration to use.
	panda_arg_list *plugin_args = panda_get_args(PLUGIN_NAME);
	char *kconf_file = g_strdup(panda_parse_string_req(plugin_args, "kconf_file", "file containing kernel configuration information"));
	char *kconf_group = g_strdup(panda_parse_string_req(plugin_args, "kconf_group", "kernel profile to use"));
	panda_free_args(plugin_args);

	// Load kernel offsets.
	if (read_kernelinfo(kconf_file, kconf_group, &ki) != 0) {
		LOG_ERROR("Failed to read group %s from %s.", kconf_group, kconf_file);
		goto error;
	}
	LOG_INFO("Read kernel info from group \"%s\" of file \"%s\".", kconf_group, kconf_file);
	g_free(kconf_file);
	g_free(kconf_group);

	if (KERNEL_VERSION(ki.version.a, ki.version.b, ki.version.c) <= KERNEL_VERSION(2, 4, 254)) {
		kernel_profile = &KERNEL24X_PROFILE;
	}

	PPP_REG_CB("osi", on_get_processes, on_get_processes);
	PPP_REG_CB("osi", on_get_process_handles, on_get_process_handles);
	PPP_REG_CB("osi", on_get_current_process, on_get_current_process);
	PPP_REG_CB("osi", on_get_process, on_get_process);
	PPP_REG_CB("osi", on_get_libraries, on_get_libraries);
	PPP_REG_CB("osi", on_get_current_thread, on_get_current_thread);
	LOG_INFO(PLUGIN_NAME " initialization complete.");
	return true;
#else
	goto error;
#endif

error:
	return false;
}

/**
 * @brief Plugin cleanup.
 */
void uninit_plugin(void *self) {
#if defined(TARGET_I386) || defined(TARGET_ARM)
	// Nothing to do...
#endif
	return;
}

/* vim:set tabstop=4 softtabstop=4 noexpandtab: */
