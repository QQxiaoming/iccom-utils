
#include <stdio.h>
#include <pty.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#include "iccom.h"

/*! Build Opt Macro */
#define BUILD_ICCSH     0
#define BUILD_ICCSHD    1

/*! Forward stdin port id */
#define ICCOM_SKIN_PORT     4080
/*! Forward stdout port id */
#define ICCOM_SKOUT_PORT    4081
/*! Forward sig port id */
#define ICCOM_SKSIG_PORT    4082

/**************************** common ****************************/
/**
 * @brief Forward data from iccom to fd
 * 
 * @param iccom_port Source iccom port num
 * @param fd Destin fd
 */
void iccom2fd_loop(unsigned int iccom_port, int fd) {
    IccomSocket sk{iccom_port};
    fd_set rfds;
    struct timeval tv{0, 0};
    char buf[4097] = {0};
    sk.open();
    sk.set_read_timeout(0);
    
    while(1) {
        if (sk.receive() >= 0) {
            int size = sk.input_size();
            for(int i = 0;i < size;i++) {
                buf[i] = sk[i];
            }
            size_t ws = write(fd, buf, size);
			fsync(fd);
        }
    }

    sk.close();
}

/**
 * @brief Forward data from fd to iccom
 * 
 * @param iccom_port Destin iccom port num
 * @param fd Source fd
 */
void fd2iccom_loop(unsigned int iccom_port, int fd) {
    IccomSocket sk{iccom_port};
    fd_set rfds;
    struct timeval tv{0, 0};
    char buf[4097] = {0};
    sk.open();
    sk.set_read_timeout(0);
    
    while(1) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if (select(fd + 1, &rfds, NULL, NULL, &tv)) {
            int size = read(fd, buf, 4096);
            sk.send_direct(buf,size);
        }
    }

    sk.close();
}

/**
 * @brief Server stdin forward handler
 */
void *sin_handler(void *arg) {
    iccom2fd_loop(ICCOM_SKIN_PORT,*(int *)arg);
    return NULL;
}

/**
 * @brief Server stdout forward handler
 */
void *sout_handler(void *arg) {
    fd2iccom_loop(ICCOM_SKOUT_PORT,*(int *)arg);
    return NULL;
}

/**
 * @brief Client stdin forward handler
 */
void *cin_handler(void *arg) {
    fd2iccom_loop(ICCOM_SKIN_PORT,*(int *)arg);
    return NULL;
}

/**
 * @brief Client stdout forward handler
 */
void *cout_handler(void *arg) {
    iccom2fd_loop(ICCOM_SKOUT_PORT,*(int *)arg);
    return NULL;
}

/**************************** iccsh ****************************/
static struct termios iccsh_stdin_termbuf_bak;
static struct termios iccsh_stdout_termbuf_bak;
static pid_t iccsh_main_pid;
static void iccsh_clean_up_and_exit(int sig)
{
    static int last_sig = 0;

    if((sig == SIGQUIT) || (last_sig == SIGTSTP)) {
        last_sig = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &iccsh_stdin_termbuf_bak);
        tcsetattr(STDOUT_FILENO, TCSANOW, &iccsh_stdout_termbuf_bak);
        kill(iccsh_main_pid, SIGKILL);
        exit(0);
    } else if(last_sig == SIGINT) {
        last_sig = 0;
        //forward sig to iccshd
        IccomSocket sk{ICCOM_SKSIG_PORT};
        sk.open();
        sk.set_read_timeout(0);
        sk.send_direct((char*)&sig,sizeof(sig));
        sk.close();
    } else {
        last_sig = sig;
    }
}

int iccsh_main(int argc, char **argv) {
    int m_stdin,m_stdout;
    int s_stdin,s_stdout;

    openpty(&m_stdin, &s_stdin, NULL, NULL, NULL);
    openpty(&m_stdout, &s_stdout, NULL, NULL, NULL);
    
    iccsh_main_pid = getpid();
    pid_t pid = fork();
    if(pid == 0) {
        close(STDERR_FILENO);

        struct termios termbuf;
        tcgetattr(STDIN_FILENO, &termbuf);
        termbuf.c_lflag &= ~ECHO;
        termbuf.c_lflag &= ~ICANON;
        termbuf.c_lflag |= IEXTEN;
        termbuf.c_lflag |= ISIG;
        termbuf.c_iflag |= ICRNL;
        termbuf.c_iflag &= ~IXOFF;
        tcsetattr(STDIN_FILENO, TCSANOW, &termbuf);

        tcgetattr(STDOUT_FILENO, &termbuf);
        termbuf.c_oflag |= ONLCR | XTABS;
        tcsetattr(STDOUT_FILENO, TCSANOW, &termbuf);

        const char *argv[] = {"/bin/sleep","1",NULL};
        execvp(argv[0], (char* const*)argv);
        while(1){
            usleep(10*1000);
        }
    } else {
        pthread_t skin, skout;
        int t_stdin = STDIN_FILENO;
        int t_stdout = STDOUT_FILENO;

        tcgetattr(STDIN_FILENO, &iccsh_stdin_termbuf_bak);
        tcgetattr(STDOUT_FILENO, &iccsh_stdout_termbuf_bak);
        signal(SIGINT, iccsh_clean_up_and_exit);
        signal(SIGTSTP, iccsh_clean_up_and_exit);
        signal(SIGQUIT, iccsh_clean_up_and_exit);

        pthread_create(&skin, NULL, cin_handler, &t_stdin);
        pthread_create(&skout, NULL, cout_handler, &t_stdout);

        pthread_join(skin, NULL);
        pthread_join(skout, NULL);
    }
    
    close(m_stdin);close(s_stdin);
    close(m_stdout);close(s_stdout);
    
    return 0;
}

/**************************** iccshd ****************************/
static pid_t iccshd_sh_pid;
static void iccshd_forward_sig(int sig)
{
    killpg(iccshd_sh_pid,SIGKILL);
}

int iccshd_main(int argc, char **argv) {
    int m_stdin,m_stdout;
    int s_stdin,s_stdout;

    openpty(&m_stdin, &s_stdin, NULL, NULL, NULL);
    openpty(&m_stdout, &s_stdout, NULL, NULL, NULL);
    
    pid_t pid = fork();
    if(pid == 0) {
    re_execvp:
        pid_t exepid;
        exepid = fork();
        if (exepid == 0) {
            setsid();
            dup2(s_stdin, STDIN_FILENO);
            dup2(s_stdout, STDOUT_FILENO);
            dup2(s_stdout, STDERR_FILENO);

            struct termios termbuf;

            tcgetattr(s_stdin, &termbuf);
            termbuf.c_lflag |= ECHO;
            termbuf.c_iflag |= ICRNL;
            termbuf.c_iflag &= ~IXOFF;
            tcsetattr(s_stdin, TCSANOW, &termbuf);

            tcgetattr(s_stdout, &termbuf);
            termbuf.c_oflag |= ONLCR | XTABS;
            tcsetattr(s_stdout, TCSANOW, &termbuf);

            const char *bash_argv[] = {"su", "-", "root", "-s", "/bin/bash",NULL};
            const char *sh_argv[] = {"su", "-", "root", "-s", "/bin/sh",NULL};
            const char **argv;
            struct stat s;
            stat("/bin/bash", &s);
            if(S_ISREG(s.st_mode) || S_ISLNK(s.st_mode)) {
                argv = bash_argv;
            } else {
                argv = sh_argv;
            }
            execvp(argv[0], (char* const*)argv);
            exit(0);
        } else {
            iccshd_sh_pid = exepid;
            signal(SIGINT, iccshd_forward_sig);
            signal(SIGTSTP, iccshd_forward_sig);
            while(1) {
                if (waitpid(exepid, NULL, WNOHANG) == exepid) {
                    goto re_execvp;
                }
            }
        }
    } else {
        pthread_t skin, skout;

        pthread_create(&skin, NULL, sin_handler, &m_stdin);
        pthread_create(&skout, NULL, sout_handler, &m_stdout);

        IccomSocket sk{ICCOM_SKSIG_PORT};
        sk.open();
        sk.set_read_timeout(0);
        while(1) {
            if (sk.receive() >= 0) {
                int size = sk.input_size();
                for(int i = 0;i < size;i++) {
                    int sig = sk[i];
                    if(sig != 0) {
                        kill(pid,sig);
                    }
                }
            }
        }
        sk.close();

        pthread_join(skin, NULL);
        pthread_join(skout, NULL);
    }
    
    close(m_stdin);close(s_stdin);
    close(m_stdout);close(s_stdout);
    return 0;
}

/**************************** main ****************************/

int main(int argc, char **argv) {
#if (BUILD_TARGET == BUILD_ICCSH)
    return iccsh_main(argc,argv);
#elif (BUILD_TARGET == BUILD_ICCSHD)
    return iccshd_main(argc,argv);
#endif
}

/**************************** end ****************************/
