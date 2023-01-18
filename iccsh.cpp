
#include <stdio.h>
#include <pty.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>

#include "iccom.h"

/*! Build Opt Macro */
#define BUILD_ICCSHD    0
#define BUILD_ICCSH     1
#define BUILD_ICCCP     2

/*! Forward stdin port id */
#define ICCOM_SKIN_PORT     4080
/*! Forward stdout port id */
#define ICCOM_SKOUT_PORT    4081
/*! Forward sig port id */
#define ICCOM_SKSIG_PORT    4082
/*! cmd port id*/
#define ICCOM_CMD_PORT      4083

/**************************** protocol ****************************/
class IccomCmdSever
{
public:
    #define RAW_HEADER_KEY         (0x42)
    #define RAW_MESSAGE_SIZE_BYTES 4096

    #define PKT_VFS_CMD   1
    #define PKT_VFS_ACK   2
    #define PKT_SYS_CMD   3
    #define PKT_SYS_ACK   4

    #define VFS_CMD_OPEN      0
    #define VFS_CMD_CLOSE     1
    #define VFS_CMD_WRITE     2
    #define VFS_CMD_READ      3
    #define VFS_CMD_LSEEK     4

    #define SYS_CMD_SYSTEM         0

    #pragma pack(push,1)
    typedef struct rawHeader {
        uint8_t key;
        uint8_t ver;
        uint8_t rsvd[6];
        uint32_t id;
        uint32_t pkt_type;
        //packet length, payload size is (length - sizeof(rawHeader))
        uint32_t length;
    }rawHeader;
    typedef struct rawVfsCmdHeader {
        rawHeader header;
        int32_t fd;
        int32_t cmd;
        uint8_t payload[0];
    } rawVfsCmdHeader;
    typedef struct rawVfsOpenCmd {
        rawVfsCmdHeader header;
        int32_t flag;
        int32_t mode;
        uint8_t path[0];
    } rawVfsOpenCmd;
    #define rawVfsCloseCmd rawVfsCmdHeader
    typedef struct rawVfsLseekCmd {
        rawVfsCmdHeader header;
        int32_t whence;
        uint32_t offset;
    } rawVfsLseekCmd;
    typedef struct rawVfsWriteCmd {
        rawVfsCmdHeader header;
        int32_t count;
        uint32_t offset;
        uint8_t data[0];
    } rawVfsWriteCmd;
    typedef struct rawVfsReadCmd {
        rawVfsCmdHeader header;
        int32_t count;
        uint32_t offset;
    } rawVfsReadCmd;
    typedef struct rawSysHeader {
	    rawHeader header;
        uint32_t cmd;
        uint8_t payload[0];
    }rawSysHeader;

    typedef struct rawSysSystem {
        rawSysHeader header;
        uint8_t data[0];
    } rawSysSystem;
    typedef struct rawVfsAckHeader {
        rawHeader header;
        int32_t ret;
        int32_t _errno;
        uint8_t payload[0];
    } rawVfsAckHeader;
    typedef struct rawVfsOpenAck {
        rawVfsAckHeader header;
        int32_t fd;
    } rawVfsOpenAck;
    #define rawVfsCloseAck rawVfsAckHeader
    typedef struct rawVfsWriteAck {
        rawVfsAckHeader header;
        int32_t count;
    } rawVfsWriteAck;
    typedef struct rawVfsReadAck {
        rawVfsAckHeader header;
        int32_t count;
        uint32_t offset;
        uint8_t data[0];
    } rawVfsReadAck;
    typedef struct rawVfsLseekAck {
        rawVfsAckHeader header;
        uint32_t offset;
    } rawVfsLseekAck;
    typedef struct rawSysAckHeader {
        rawHeader header;
        int32_t ret;
        int32_t _errno;
        uint8_t payload[0];
    } rawSysAckHeader;

    #define rawSysSystemAck rawSysAckHeader
    #pragma pack(pop)

    IccomCmdSever(uint16_t port) {
        _sock = new IccomSocket(port);
    }

    virtual ~IccomCmdSever() {
        _sock->close();
        delete _sock;
    }

    IccomCmdSever(const IccomCmdSever &){}
    void operator=(const IccomCmdSever &){}

    int Init(void) {
        _nSendId = 0;
        _nRepeatLen = 0;
        int ret = _sock->open();
        _sock->set_read_timeout(1000);
        _sock->set_write_timeout(1000);
        return ret;
    }

    int DeInit() {
        _sock->close();
        return 0;
    }

    int Handler() {
        uint32_t nLen;

        while (1) {
            if (ReceiveMsg(nLen) < 0) {
                continue;
            }

            if (!isRawHeader(_cRecvData)) {
                continue;
            }

            switch (getRawHeaderType(_cRecvData)) {
                case PKT_VFS_CMD:
                    nLen = VFSAck();
                    break;
                case PKT_SYS_CMD:
                    nLen = SYSAck();
                    break;
                default:
                    nLen = 0;
                    break;
            }

            if (nLen > 0) {
                _sock->send_direct(_cSendData,nLen);
            }
        }

        return 0;
    }

    int SendVFSOpen(const char *pathname, int flags, mode_t mode) {
        rawVfsOpenCmd *h = (rawVfsOpenCmd *)_cSendData;
        h->flag = flags;
        h->mode = mode;
        memcpy(&h->path, pathname, strlen(pathname));
        rawHeader *sendRaw = initRawVfsCmdHeader(h, _nSendId++, -1, VFS_CMD_OPEN, strlen(pathname) + sizeof(*h));

        if (0 == SendAndCheckAck()) {
            rawVfsOpenAck* recv = (rawVfsOpenAck*)_cRecvData;
            if(recv->header.ret < 0) {
                errno = recv->header._errno;
                return recv->header.ret;
            } else {
                return recv->fd;
            }
        }
        return -EPIPE;
    }

    int SendVFSClose(int fd) {
        rawHeader *sendRaw = initRawVfsCmdHeader(_cSendData, _nSendId++, fd, VFS_CMD_CLOSE,sizeof(rawVfsCmdHeader));
        if (0 == SendAndCheckAck()) {
            rawVfsCloseAck* recv = (rawVfsCloseAck*)_cRecvData;
            if(recv->ret < 0) {
                errno = recv->_errno;
            }
            return recv->ret;
        }
        return -EPIPE;
    }

    ssize_t SendVFSRead(int fd, void *buf, size_t count, off_t offset) {
        rawVfsReadCmd *h = (rawVfsReadCmd *)_cSendData;
        h->count = count;
        h->offset = offset;
        rawHeader *sendRaw = initRawVfsCmdHeader(h, _nSendId++, fd, VFS_CMD_READ, sizeof(*h));
        if (0 == SendAndCheckAck()) {
            rawVfsReadAck* recv = (rawVfsReadAck*)_cRecvData;
            if(recv->header.ret < 0) {
                errno = recv->header._errno;
                return recv->header.ret;
            } else {
                memcpy(buf,recv->data,recv->count);
                return recv->count;
            }
        }
        return -EPIPE;
    }

    ssize_t SendVFSWrite(int fd, const void *buf, size_t count, off_t offset) {
        rawVfsWriteCmd *h = (rawVfsWriteCmd *)_cSendData;
        h->count = count;
        h->offset = offset;
        memcpy(&h->data, buf, count);
        rawHeader *sendRaw = initRawVfsCmdHeader(h, _nSendId++, fd, VFS_CMD_WRITE, count + sizeof(*h));

        if (0 == SendAndCheckAck()) {
            rawVfsWriteAck* recv = (rawVfsWriteAck*)_cRecvData;
            if(recv->header.ret < 0) {
                errno = recv->header._errno;
                return recv->header.ret;
            } else {
                return recv->count;
            }
        }
        return -EPIPE;
    }

    off_t SendVFSLseek(int fd, off_t offset, int whence) {
        rawVfsLseekCmd *h = (rawVfsLseekCmd *)_cSendData;
        h->whence = whence;
        h->offset = offset;
        rawHeader *sendRaw = initRawVfsCmdHeader(h, _nSendId++, fd, VFS_CMD_LSEEK, sizeof(*h));
        if (0 == SendAndCheckAck()) {
            rawVfsLseekAck* recv = (rawVfsLseekAck*)_cRecvData;
            if(recv->header.ret < 0) {
                errno = recv->header._errno;
                return recv->header.ret;
            } else {
                return recv->offset;
            }
        }
        return -EPIPE;
    }

    int SendSYSSystem(const char *str) {
        rawSysSystem *h = (rawSysSystem *)_cSendData;
        uint32_t len = strlen(str);
        memcpy(&h->data, str, len);
        rawHeader *sendRaw = initRawSysHeader(_cSendData, _nSendId++, SYS_CMD_SYSTEM, (sizeof(*h) + len));
        if (0 == SendAndCheckAck()) {
            rawSysSystemAck* recv = (rawSysSystemAck*)_cRecvData;
            if(recv->ret < 0) { 
                errno = recv->_errno;
            } 
            return recv->ret;
        }
        return -EPIPE;
    }

private:
    rawHeader *initRawHeader(void *buff, uint32_t id, uint32_t type, uint32_t len) {
        rawHeader *h = (rawHeader *)buff;
        memset(h, 0, sizeof(rawHeader));
        h->key = RAW_HEADER_KEY;
        h->id = id;
        h->pkt_type = type;
        h->length = len;
        return h;
    }

    rawHeader *initRawVfsCmdHeader(void *buff, uint32_t id, int32_t fd, int32_t cmd, uint32_t len) {
        rawVfsCmdHeader *h = (rawVfsCmdHeader *)buff;
        h->fd = fd;
        h->cmd = cmd;
        return initRawHeader(buff, id, PKT_VFS_CMD, len);
    }

    rawHeader *initRawSysHeader(void *buff, uint32_t id, uint32_t cmd, uint32_t len) {
        rawSysHeader *h = (rawSysHeader *)buff;
        h->cmd = cmd;
        return initRawHeader(buff, id, PKT_SYS_CMD, len);
    }

    rawHeader *initRawVfsAckHeader(void *buff, uint32_t id, int32_t ret,
                                                int32_t err, uint32_t len) {
        rawVfsAckHeader *h = (rawVfsAckHeader *)buff;
        h->ret = ret;
        h->_errno = err;
        return initRawHeader(buff, id, PKT_VFS_ACK, len);
    }

    rawHeader *initRawSysAckHeader(void *buff, uint32_t id, int32_t ret, int32_t err, uint32_t len) {
        rawSysAckHeader *h = (rawSysAckHeader *)buff;
        h->ret = ret;
        h->_errno = err;
        return initRawHeader(buff, id, PKT_SYS_ACK, len);
    }

    bool isRawHeader(void *buff) {
        rawHeader *h = (rawHeader *)buff;
        return (h->key == RAW_HEADER_KEY);
    }

    uint32_t getRawHeaderType(void *buff) {
        rawHeader *h = (rawHeader *)buff;
        return h->pkt_type;
    }

    uint32_t getRawHeaderId(void *buff) {
        rawHeader *h = (rawHeader *)buff;
        return h->id;
    }

    int32_t getVfsCmd(void *buff) {
        rawVfsCmdHeader *h = (rawVfsCmdHeader *)buff;
        return h->cmd;
    }

    int32_t getSysCmd(void *buff) {
        rawSysHeader *h = (rawSysHeader *)buff;
        return h->cmd;
    }

    uint32_t VFSAck(void){
        rawHeader *sendRaw = (rawHeader *)_cSendData;
        sendRaw->length = 0;

        switch (getVfsCmd(_cRecvData))
        {
        case VFS_CMD_OPEN: {
            rawVfsOpenCmd *cmd = (rawVfsOpenCmd *)_cRecvData;
            int _err = 0,_fd = 0;
            int _ret = open((const char *)cmd->path, cmd->flag, cmd->mode);
            if(_ret < 0) {
                _err = errno;
            } else {
                _fd = _ret;
            }
            rawVfsOpenAck *h = (rawVfsOpenAck *)_cSendData;
            h->fd = _fd;
            sendRaw = initRawVfsAckHeader(_cSendData, getRawHeaderId(_cRecvData), _ret, _err, sizeof(*h));
            break;
        }
        case VFS_CMD_CLOSE: {
            rawVfsCloseCmd *cmd = (rawVfsCloseCmd *)_cRecvData;
            int _err = 0;
            int _ret = close(cmd->fd);
            if(_ret != 0) {
                _err = errno;
            }
            sendRaw = initRawVfsAckHeader(_cSendData, getRawHeaderId(_cRecvData), _ret, _err, sizeof(rawVfsCloseAck));
            break;
        }
        case VFS_CMD_WRITE: {
            rawVfsWriteCmd *cmd = (rawVfsWriteCmd *)_cRecvData;
            int _err = 0,_cnt = 0;
            int _ret = lseek(cmd->header.fd,cmd->offset,SEEK_SET);
            if(_ret == cmd->offset) {
                _ret = write(cmd->header.fd, cmd->data, cmd->count);
                if(_ret < 0) {
                    _err = errno;
                } else {
                    _cnt = _ret;
                }
            } else {
                _ret = -1;
                _err = errno;
            }
            rawVfsWriteAck *h = (rawVfsWriteAck *)_cSendData;
            h->count = _cnt;
            sendRaw = initRawVfsAckHeader(_cSendData, getRawHeaderId(_cRecvData), _ret, _err, sizeof(*h));
            break;
        }
        case VFS_CMD_READ:{
            rawVfsReadCmd *cmd = (rawVfsReadCmd *)_cRecvData;
            uint8_t *read_buf = ((rawVfsReadAck *)sendRaw)->data;
            int _err = 0,_cnt = 0;
            int _ret = lseek(cmd->header.fd,cmd->offset,SEEK_SET);
            if(_ret == cmd->offset) {
                _ret = read(cmd->header.fd, read_buf, cmd->count);
                if(_ret < 0) {
                    _err = errno;
                } else {
                    _cnt = _ret;
                }
            } else {
                _ret = -1;
                _err = errno;
            }
            rawVfsReadAck *h = (rawVfsReadAck *)_cSendData;
            h->count = _cnt;
            h->offset = cmd->offset+_cnt;
            memcpy(&h->data, read_buf, _cnt);
            sendRaw = initRawVfsAckHeader(_cSendData, getRawHeaderId(_cRecvData), _ret, _err, _cnt + sizeof(*h));
            break;
        }
        case VFS_CMD_LSEEK:{
            rawVfsLseekCmd *cmd = (rawVfsLseekCmd *)_cRecvData;
            int _ret = lseek(cmd->header.fd,cmd->offset,cmd->whence);
            int _err = 0,_off = 0;
            if(_ret < 0) {
                _err = errno;
            } else {
                _off = _ret;
            }
            rawVfsLseekAck *h = (rawVfsLseekAck *)_cSendData;
            h->offset = _off;
            sendRaw = initRawVfsAckHeader(_cSendData, getRawHeaderId(_cRecvData), _ret, _err, sizeof(*h));
            break;
        }
        default:
            sendRaw = initRawVfsAckHeader(_cSendData, getRawHeaderId(_cRecvData), -EINVAL, EINVAL, sizeof(rawVfsAckHeader));
            break;
        }

        return sendRaw->length;
    }

    uint32_t SYSAck(void) {
        rawHeader *sendRaw = (rawHeader *)_cSendData;
        sendRaw->length = 0;

        switch (getSysCmd(_cRecvData))
        {
        case SYS_CMD_SYSTEM: {
            rawSysSystem *cmd = (rawSysSystem *)_cRecvData;
            int _err = 0;
            int _ret = system((char *)cmd->data);
            if(_ret != 0) {
                _err = errno;
            }
            sendRaw = initRawSysAckHeader(_cSendData, getRawHeaderId(_cRecvData), _ret, _err, sizeof(rawSysSystemAck));
            break;
        }
        default:
            sendRaw = initRawSysAckHeader(_cSendData, getRawHeaderId(_cRecvData), -EINVAL, EINVAL, sizeof(rawSysAckHeader));
            break;
        }
        return sendRaw->length;
    }

    int SendAndCheckAck(void) {
        int retry_index = 0;
        rawHeader *sendRaw = (rawHeader *)_cSendData;
        if (sendRaw->length > 0) {
            int ret = _sock->send_direct(_cSendData,sendRaw->length);
            if(ret == 0) {
                do {
                    ret = _sock->receive_direct(_cRecvData,RAW_MESSAGE_SIZE_BYTES);
                    retry_index++;
                } while(ret <= 0 && retry_index < 5);
                if(isRawHeader(_cRecvData) &&
                    getRawHeaderId(_cRecvData) == sendRaw->id && 
                    getRawHeaderType(_cRecvData) == sendRaw->pkt_type+1) {
                    return 0;
                }
            }
        }
        return -EPIPE;
    }

    int ReceiveMsg(uint32_t& nLen){
        int ret = _sock->receive_direct(_cRecvData,RAW_MESSAGE_SIZE_BYTES);
        if(ret <= 0) {
            nLen = 0;
            return -1;
        } else {
            nLen = ret;
            if(nLen < RAW_MESSAGE_SIZE_BYTES)
                _cRecvData[nLen] = 0;
            return 0;
        }
    }

public:
    char                 _cRecvData[RAW_MESSAGE_SIZE_BYTES];
    char                 _cSendData[RAW_MESSAGE_SIZE_BYTES];
    char                 _cRepeatData[RAW_MESSAGE_SIZE_BYTES];

private:
	IccomSocket          *_sock; 
    uint32_t             _nRepeatLen;
    uint32_t             _nSendId;
};

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
 * @brief Client stdin forward handler
 */
void *cin_handler(void *arg) {
    fd2iccom_loop(ICCOM_SKIN_PORT,*(int *)arg);
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
 * @brief Client stdout forward handler
 */
void *cout_handler(void *arg) {
    iccom2fd_loop(ICCOM_SKOUT_PORT,*(int *)arg);
    return NULL;
}

void *ssig_handler(void *arg) {
    pid_t pid = *(pid_t *)arg;
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
    return NULL;
}

void *csig_handler(void *arg) {
    int sig = *(int *)arg;
    IccomSocket sk{ICCOM_SKSIG_PORT};
    sk.open();
    sk.set_read_timeout(0);
    sk.send_direct((char*)&sig,sizeof(sig));
    sk.close();
    return NULL;
}

void *scmd_handler(void *arg) {
    IccomCmdSever sk(ICCOM_CMD_PORT);
    sk.Init();
    sk.Handler();
    sk.DeInit();
    return NULL;
}

/**************************** iccshd ****************************/
static pid_t iccshd_sh_pid;
static void iccshd_forward_sig(int sig)
{
    killpg(iccshd_sh_pid,SIGKILL);
}

static void iccshd_clean_up_and_exit(int sig)
{
    killpg(getpid(),SIGKILL);
}

int iccshd_main(int argc, char **argv) {
    int m_stdin,m_stdout;
    int s_stdin,s_stdout;

    setsid();
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
        signal(SIGINT, iccshd_clean_up_and_exit);
        signal(SIGTSTP, iccshd_clean_up_and_exit);

        pthread_t skin, skout, sksig, skcmd;

        pthread_create(&skin, NULL, sin_handler, &m_stdin);
        pthread_create(&skout, NULL, sout_handler, &m_stdout);
        pthread_create(&sksig, NULL, ssig_handler, &pid);
        pthread_create(&skcmd, NULL, scmd_handler, NULL);

        pthread_join(skin, NULL);
        pthread_join(skout, NULL);
        pthread_join(sksig, NULL);
        pthread_join(skcmd, NULL);
    }
    
    close(m_stdin);close(s_stdin);
    close(m_stdout);close(s_stdout);
    return 0;
}

/**************************** iccsh ****************************/
static struct termios iccsh_stdin_termbuf_bak;
static struct termios iccsh_stdout_termbuf_bak;
static void iccsh_clean_up_and_exit(int sig)
{
    static int last_sig = 0;

    if((sig == SIGQUIT) || (last_sig == SIGTSTP)) {
        last_sig = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &iccsh_stdin_termbuf_bak);
        tcsetattr(STDOUT_FILENO, TCSANOW, &iccsh_stdout_termbuf_bak);
        kill(getpid(), SIGKILL);
        exit(0);
    } else if(last_sig == SIGINT) {
        last_sig = 0;
        //forward sig to iccshd
        csig_handler(&sig);
    } else {
        last_sig = sig;
    }
}

int iccsh_main(int argc, char **argv) {
    int m_stdin,m_stdout;
    int s_stdin,s_stdout;

    openpty(&m_stdin, &s_stdin, NULL, NULL, NULL);
    openpty(&m_stdout, &s_stdout, NULL, NULL, NULL);
    
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

/**************************** icccp ****************************/
void icccp_useage(void)
{
    printf("USEAGE:\t icccp SRC([Address]:[Path]) DEST([Address]:[Path]) [-f] [-r]\n");
    printf("\t remote must use full path!\n");
    printf("e.g.:\t icccp local:srcfile remote:/<full path>/destfile\n");
    printf("\t icccp remote:/<full path>/srcfile local:destfile\n");
    printf("\t icccp local:srcdir remote:/<full path>/destdir -r\n");
    printf("\t icccp remote:/<full path>/destdir local:srcdir -r\n");
}

int remote_sync_file_write(IccomCmdSever &dev,const char *srcfillname,const char *destfillname,
    bool force,bool recursive)
{
    bool is_dir = false;
    int size = strlen(srcfillname) + 10;
    char *cmd = (char *)malloc(size);
    if(cmd){
        sprintf(cmd,"[ -d \"%s\" ]",srcfillname);
        int ret = system((const char *)cmd);
        if(ret == 0) {
            is_dir = true;
        }
        free(cmd);
    } else {
        printf("malloc fail!\n");
        return -1;
    }
    if(is_dir) {
        if(recursive) {
            printf(
                    "Recursive is not supported for now!\n"
                    "Please contact me at https://github.com/QQxiaoming/iccom-utils/issues.\n"
                  );
            return -1;
        } else {
            icccp_useage();
            exit(-1);
        }
    } else {
        int tfd = dev.SendVFSOpen(destfillname,O_RDONLY,0);
        if(tfd > 0) {
            dev.SendVFSClose(tfd);
            if(!force){
                printf("%s already exists!\n",destfillname);
                return -1;
            }
            int size = strlen(destfillname) + 4;
            char *cmd = (char *)malloc(size);
            if(cmd){
                sprintf(cmd,"rm %s",destfillname);
                dev.SendSYSSystem((const char *)cmd);
                free(cmd);
            } else {
                printf("malloc fail!\n");
                return -1;
            }
        } 
        
        uint8_t data[2048];
        FILE * fp = NULL;
        int file_size = 0;
        fp = fopen(srcfillname, "rb");
        if (!fp) {
            printf("fopen fail!\n");
            return -1;
        }
        fseek(fp, 0, SEEK_END);
        file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        int fd = dev.SendVFSOpen(destfillname, O_WRONLY | O_NONBLOCK | O_CREAT, 0);
        if(fd){
            for(uint32_t send_size = 0; send_size < file_size;) {
                uint32_t size = fread(data, 1, 2048, fp);
                if(size) {
                    fflush(stdout);
                    int _ret =dev.SendVFSWrite(fd,data,size,send_size);
                    if(_ret != size) {
                        dev.SendVFSClose(fd);
                        fclose(fp);
                        printf("SendVFSWrite fail %d!\n",_ret);
                        return -1;
                    }
                    send_size += size;
                } else {
                    break;
                }
            }
        } else {
            printf("create %s fail!\n",destfillname);
        }

        dev.SendVFSClose(fd);
        fclose(fp);
        dev.SendSYSSystem("sync");
        return 0;
    }
}

int remote_sync_file_read(IccomCmdSever &dev,const char *srcfillname,const char *destfillname, 
    bool force,bool recursive)
{
    bool is_dir = false;
    int size = strlen(srcfillname) + 10;
    char *cmd = (char *)malloc(size);
    if(cmd){
        sprintf(cmd,"[ -d \"%s\" ]",srcfillname);
        int ret = dev.SendSYSSystem((const char *)cmd);
        if(ret == 0) {
            is_dir = true;
        }
        free(cmd);
    } else {
        printf("malloc fail!\n");
        return -1;
    }
    if(is_dir) {
        if(recursive) {
            printf(
                    "Recursive is not supported for now!\n"
                    "Please contact me at https://github.com/QQxiaoming/iccom-utils/issues.\n"
                  );
            return -1;
        } else {
            icccp_useage();
            exit(-1);
        }
    } else {
        FILE * fp = NULL;
        fp = fopen(destfillname, "rb");
        if(fp) {
            fclose(fp);
            if(!force){
                printf("%s already exists!\n",destfillname);
                return -1;
            }
            int size = strlen(destfillname) + 4;
            char *cmd = (char *)malloc(size);
            if(cmd){
                sprintf(cmd,"rm %s",destfillname);
                int _ret = system((const char *)cmd);
                free(cmd);
            } else {
                printf("malloc fail!\n");
                return -1;
            }
        } 
        
        uint8_t data[2048];
        int file_size = 0;
        int tfd = dev.SendVFSOpen(srcfillname,O_RDONLY,0);
        if (tfd<=0) {
            printf("SendVFSOpen fail!\n");
            return -1;
        }
        file_size = dev.SendVFSLseek(tfd, 0, SEEK_END);
        if(file_size == -1){
            printf("SendVFSLseek fail!\n");
            dev.SendVFSClose(tfd);
            return -1;
        }
        dev.SendVFSLseek(tfd, 0, SEEK_SET);

        int fd = open(destfillname, O_WRONLY | O_NONBLOCK | O_CREAT, 0);
        if(fd){
            for(uint32_t recv_size = 0; recv_size < file_size;) {
                int32_t size = dev.SendVFSRead(tfd,data, 2048, recv_size);
                if(size) {
                    fflush(stdout);
                    size_t ws = write(fd,data,size);
                    recv_size += size;
                } else {
                    dev.SendVFSClose(tfd);
                    close(fd);
                    printf("\nSendVFSRead fail %d!\n",size);
                    break;
                }
            }
        } else {
            printf("create %s fail!\n",destfillname);
        }

        close(fd);
        dev.SendVFSClose(tfd);
        int sr = system("sync");
        return 0;
    }
}

int icccp_main(int argc, char **argv)
{        
    IccomCmdSever sk(ICCOM_CMD_PORT);
    int ret = 0;
    bool force_sync = false;
    bool send = false;
    bool recv = false;
    bool recursive = false;
    char *srcfile;
    char *destfile;

    if(argc < 3) {
        printf("useage fail!\n");
        icccp_useage();
        exit(-1);
    }

    for(int i = 3; i < argc; i++) {
        if(strcmp(argv[i], "-f") == 0){
            force_sync = true;
        }
        if(strcmp(argv[i], "-r") == 0){
            recursive = true;
        }
        if(strcmp(argv[i], "-R") == 0){
            recursive = true;
        }
        if(strcmp(argv[i], "-h") == 0){
            icccp_useage();
            exit(0);
        }
    }

    if((strncmp(argv[1],"local:",6) == 0)&&(strncmp(argv[2],"remote:",7) == 0)) {
        send = true;
        srcfile = &argv[1][6];
        destfile = &argv[2][7];
    }
    if((strncmp(argv[1],"remote:",7) == 0)&&(strncmp(argv[2],"local:",6) == 0)) {
        recv = true;
        srcfile = &argv[1][7];
        destfile = &argv[2][6];
    }

    if((send && recv)||(!send && !recv)) {
        icccp_useage();
        exit(-1);
    }

    sk.Init();
    if(send) {
        ret = remote_sync_file_write(sk,srcfile,destfile,force_sync,recursive);
    }
    if(recv) {
        ret = remote_sync_file_read(sk,srcfile,destfile,force_sync,recursive);
    }
    sk.DeInit();

    return ret;
}

/**************************** main ****************************/
int main(int argc, char **argv) {
#if (BUILD_TARGET == BUILD_ICCSH)
    return iccsh_main(argc,argv);
#elif (BUILD_TARGET == BUILD_ICCCP)
    return icccp_main(argc,argv);
#elif (BUILD_TARGET == BUILD_ICCSHD)
    return iccshd_main(argc,argv);
#endif
}

/**************************** end ****************************/
