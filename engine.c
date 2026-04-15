#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define STACK_SIZE (1024*1024)
#define CONTAINER_ID_LEN 32

typedef enum { CMD_START, CMD_STOP, CMD_LOGS } command_kind_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[256];
    char command[128];
} control_request_t;

typedef struct {
    int status;
    char message[4096];
} control_response_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    int pipe_fd;
    struct container_record *next;
} container_record_t;

container_record_t *head=NULL;

void monitor_register(pid_t pid,const char *id)
{
    int fd=open("/dev/monitor",O_RDWR);
    if(fd<0) return;

    struct monitor_request req;

    req.pid=pid;
    req.soft_limit_bytes=0;
    req.hard_limit_bytes=0;

    strncpy(req.container_id,id,MONITOR_NAME_LEN-1);
    req.container_id[MONITOR_NAME_LEN-1]='\0';

    ioctl(fd,MONITOR_REGISTER,&req);

    close(fd);
}

void monitor_unregister(pid_t pid,const char *id)
{
    int fd=open("/dev/monitor",O_RDWR);
    if(fd<0) return;

    struct monitor_request req;

    req.pid=pid;

    strncpy(req.container_id,id,MONITOR_NAME_LEN-1);
    req.container_id[MONITOR_NAME_LEN-1]='\0';

    ioctl(fd,MONITOR_UNREGISTER,&req);

    close(fd);
}

void add_container(const char *id,pid_t pid,int pipefd)
{
    container_record_t *node=malloc(sizeof(container_record_t));

    strncpy(node->id,id,CONTAINER_ID_LEN-1);
    node->pid=pid;
    node->pipe_fd=pipefd;

    node->next=head;
    head=node;
}

container_record_t* find_container(const char *id)
{
    container_record_t *curr=head;

    while(curr)
    {
        if(strcmp(curr->id,id)==0)
            return curr;

        curr=curr->next;
    }

    return NULL;
}

void remove_container(const char *id)
{
    container_record_t *curr=head,*prev=NULL;

    while(curr)
    {
        if(strcmp(curr->id,id)==0)
        {
            if(prev)
                prev->next=curr->next;
            else
                head=curr->next;

            close(curr->pipe_fd);
            free(curr);
            return;
        }

        prev=curr;
        curr=curr->next;
    }
}

typedef struct{
    char rootfs[256];
    char command[128];
    int pipefd[2];
} child_args_t;

int child_fn(void *arg)
{
    child_args_t *cargs=(child_args_t*)arg;

    dup2(cargs->pipefd[1],STDOUT_FILENO);
    dup2(cargs->pipefd[1],STDERR_FILENO);

    close(cargs->pipefd[0]);
    close(cargs->pipefd[1]);

    chroot(cargs->rootfs);
    chdir("/");

    mkdir("/proc",0555);
    mount("proc","/proc","proc",0,NULL);

    execl(cargs->command,cargs->command,NULL);

    perror("exec failed");
    return 1;
}

static int run_supervisor()
{
    int server_fd,client_fd;
    struct sockaddr_un addr;

    unlink(CONTROL_PATH);

    server_fd=socket(AF_UNIX,SOCK_STREAM,0);

    memset(&addr,0,sizeof(addr));
    addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path,CONTROL_PATH,sizeof(addr.sun_path)-1);

    bind(server_fd,(struct sockaddr*)&addr,sizeof(addr));
    listen(server_fd,5);

    printf("Supervisor running...\n");

    while(1)
    {
        control_request_t req;
        control_response_t res;

        client_fd=accept(server_fd,NULL,NULL);

        read(client_fd,&req,sizeof(req));

        if(req.kind==CMD_START)
        {
            int pipefd[2];
            pipe(pipefd);

            child_args_t *cargs=malloc(sizeof(child_args_t));

            strcpy(cargs->rootfs,req.rootfs);
            strcpy(cargs->command,req.command);

            cargs->pipefd[0]=pipefd[0];
            cargs->pipefd[1]=pipefd[1];

            void *stack=malloc(STACK_SIZE);
            void *stack_top=stack+STACK_SIZE;

            pid_t pid=clone(child_fn,stack_top,
                CLONE_NEWPID|CLONE_NEWUTS|CLONE_NEWNS|SIGCHLD,
                cargs);

            close(pipefd[1]);

            add_container(req.container_id,pid,pipefd[0]);

            monitor_register(pid,req.container_id);

            snprintf(res.message,sizeof(res.message),
                     "Started container %s PID=%d",
                     req.container_id,pid);
        }

        else if(req.kind==CMD_STOP)
        {
            container_record_t *c=find_container(req.container_id);

            if(c)
            {
                kill(c->pid,SIGKILL);

                monitor_unregister(c->pid,req.container_id);

                remove_container(req.container_id);

                snprintf(res.message,sizeof(res.message),
                         "Stopped container %s",
                         req.container_id);
            }
        }

        write(client_fd,&res,sizeof(res));
        close(client_fd);

        container_record_t *curr=head;

        while(curr)
        {
            char buf[256];
            int n=read(curr->pipe_fd,buf,sizeof(buf)-1);

            if(n>0)
            {
                buf[n]='\0';
                printf("[LOG %s]: %s",curr->id,buf);
            }

            curr=curr->next;
        }
    }
}

static int send_control_request(control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t res;

    sock=socket(AF_UNIX,SOCK_STREAM,0);

    memset(&addr,0,sizeof(addr));
    addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path,CONTROL_PATH,sizeof(addr.sun_path)-1);

    connect(sock,(struct sockaddr*)&addr,sizeof(addr));

    write(sock,req,sizeof(*req));
    read(sock,&res,sizeof(res));

    printf("%s\n",res.message);

    close(sock);

    return 0;
}

int main(int argc,char *argv[])
{
    if(strcmp(argv[1],"supervisor")==0)
        return run_supervisor();

    if(strcmp(argv[1],"start")==0)
    {
        control_request_t req;

        req.kind=CMD_START;

        strcpy(req.container_id,argv[2]);
        strcpy(req.rootfs,argv[3]);
        strcpy(req.command,argv[4]);

        return send_control_request(&req);
    }

    if(strcmp(argv[1],"stop")==0)
    {
        control_request_t req;

        req.kind=CMD_STOP;

        strcpy(req.container_id,argv[2]);

        return send_control_request(&req);
    }

    return 0;
}
