#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "monitor"

MODULE_LICENSE("GPL");

typedef struct proc_node {
    pid_t pid;
    char id[MONITOR_NAME_LEN];
    struct proc_node *next;
} proc_node_t;

static proc_node_t *head=NULL;
static DEFINE_MUTEX(list_lock);
static int major;

static int register_process(struct monitor_request *req)
{
    proc_node_t *node;

    node=kmalloc(sizeof(proc_node_t),GFP_KERNEL);
    if(!node) return -ENOMEM;

    node->pid=req->pid;

    strncpy(node->id,req->container_id,MONITOR_NAME_LEN-1);
    node->id[MONITOR_NAME_LEN-1]='\0';

    mutex_lock(&list_lock);

    node->next=head;
    head=node;

    mutex_unlock(&list_lock);

    printk(KERN_INFO "monitor: registered pid=%d id=%s\n",
           node->pid,node->id);

    return 0;
}

static int unregister_process(struct monitor_request *req)
{
    proc_node_t *curr=head,*prev=NULL;

    mutex_lock(&list_lock);

    while(curr)
    {
        if(curr->pid==req->pid)
        {
            if(prev)
                prev->next=curr->next;
            else
                head=curr->next;

            printk(KERN_INFO "monitor: unregistered pid=%d id=%s\n",
                   curr->pid,curr->id);

            kfree(curr);

            mutex_unlock(&list_lock);

            return 0;
        }

        prev=curr;
        curr=curr->next;
    }

    mutex_unlock(&list_lock);

    return -EINVAL;
}

static long monitor_ioctl(struct file *file,
                          unsigned int cmd,
                          unsigned long arg)
{
    struct monitor_request req;

    if(copy_from_user(&req,
       (struct monitor_request __user *)arg,
       sizeof(req)))
        return -EFAULT;

    switch(cmd)
    {
        case MONITOR_REGISTER:
            return register_process(&req);

        case MONITOR_UNREGISTER:
            return unregister_process(&req);

        default:
            return -EINVAL;
    }
}

static struct file_operations fops={
    .owner=THIS_MODULE,
    .unlocked_ioctl=monitor_ioctl,
};

static int __init monitor_init(void)
{
    major=register_chrdev(0,DEVICE_NAME,&fops);

    if(major<0)
    {
        printk(KERN_ALERT "monitor: failed to register\n");
        return major;
    }

    printk(KERN_INFO "monitor: loaded with major %d\n",major);

    return 0;
}

static void __exit monitor_exit(void)
{
    unregister_chrdev(major,DEVICE_NAME);

    printk(KERN_INFO "monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
