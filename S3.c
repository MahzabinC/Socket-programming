#include <netinet/in.h> //structure for storing address information
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h> //for socket APIs
#include <sys/types.h>
#include <string.h>
#include <unistd.h>    // declares fork(), pipe(), read(), write(), etc.
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>  //for inet_pton()
//#include <libkern/OSByteOrder.h> //be64toh htobe64
#include <endian.h>  // linux
#include <dirent.h>
#include <ftw.h>

char collected_files[100][256];
int file_count = 0;

#define PORT 6668
#define BUFFER_SIZE 4096
#define SERVER_IP "127.0.0.1"

// Utility: Read and write helpers
uint32_t read_uint32(int socket) {
    uint32_t val;
    read(socket, &val, sizeof(val));
    return ntohl(val);
}
uint64_t read_uint64(int socket) {
    uint64_t val;
    read(socket, &val, sizeof(val));
    return be64toh(val);
    //return OSSwapBigToHostInt64(val);
}
void write_uint32(int socket, uint32_t length) {
    length = htonl(length);
    write(socket, &length, sizeof(length));
}

void write_uint64(int sock, uint64_t val) {
    //val = OSSwapHostToBigInt64(val);
    val = htobe64(val);
    write(sock, &val, sizeof(val));
}
//Create directories recursively
int mkdir_p(const char *path) {
    char temp[512];
    char *p = NULL;
    size_t len;

    // Expand tilde if needed
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) return -1;
        snprintf(temp, sizeof(temp), "%s/%s", home, path + 1);
        path = temp;
    }
    strncpy(temp, path, sizeof(temp));
    len = strlen(temp);
    // Remove trailing slash
    if (temp[len - 1] == '/') {
        temp[len - 1] = '\0';
    }
    for (p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
                perror("mkdir");
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }
    return 0;
}

void replace_path(const char *input, char *output, size_t outsize) {
    const char *prefix1 = "~S1/";
    const char *prefix2 = "S1/";
    const char *prefix3 = "~S1";
    const char *prefix4 = "S1";
    const char *replacement1 = "~S3/";
    const char *replacement2 = "S3/";
    const char *replacement3 = "~S3";
    const char *replacement4 = "S3";

    if (strncmp(input, prefix1, strlen(prefix1)) == 0) {
        snprintf(output, outsize, "%s%s", replacement1, input + strlen(prefix1));
    }
    else if (strncmp(input, prefix2, strlen(prefix2)) == 0) {
        snprintf(output, outsize, "%s%s", replacement2, input + strlen(prefix2));
    }
    else if (strncmp(input, prefix3, strlen(prefix3)) == 0) {
        snprintf(output, outsize, "%s%s", replacement3, input + strlen(prefix3));
    }
    else if (strncmp(input, prefix4, strlen(prefix4)) == 0) {
        snprintf(output, outsize, "%s%s", replacement4, input + strlen(prefix4));
    }
    else {
        snprintf(output, outsize, "%s", input);  // no match, copy as is
    }
}
void uploadf(int socket){
    char destination[256]={0};
    char filename[256]={0};

    uint32_t filename_size = read_uint32(socket);
    read(socket, filename, filename_size);
    uint32_t destination_size = read_uint32(socket);
    read(socket, destination, destination_size);
    uint64_t file_size = read_uint64(socket);
    char temp[512];
    replace_path(destination,temp,sizeof(temp));
    // Expand ~ to home directory
    if (temp[0] == '~') {
        const char *home = getenv("HOME");
        if (home) {
            char expanded[512];
            snprintf(expanded, sizeof(expanded), "%s/%s", home, temp + 1);
            strcpy(temp, expanded);
        }
    }
    mkdir_p(temp); // ensure dir exists
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", temp, filename); //full path


    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char buffer[BUFFER_SIZE];
    while (file_size > 0) {
        const ssize_t rb = read(socket, buffer, file_size > BUFFER_SIZE ? BUFFER_SIZE : file_size);
        if (rb <= 0) break;
        const ssize_t wb = write(fd, buffer, rb);
        if (wb != rb) {
            perror("write mismatch");
            close(fd);
            close(socket);
            return;
        }
        file_size -= rb;
        memset(buffer,0,sizeof(buffer));
    }
    close(fd);
    printf("S3 Stored %s locally\n", filename);
}

void downloadf(int socket){
  char path[256]={0};
  uint32_t path_size = read_uint32(socket);
  read(socket, path, path_size);
  char mod_path[512];
  replace_path(path,mod_path,sizeof(mod_path));
    if (mod_path[0] == '~') {
        const char *home = getenv("HOME");
        if (home) {
            char expanded[512];
            snprintf(expanded, sizeof(expanded), "%s/%s", home, mod_path + 1);
            strcpy(mod_path, expanded);
        }
    }
  int fd = open(mod_path, O_RDONLY);
  char meow[BUFFER_SIZE];
  // getting file size and sending it in socket
  struct stat st;
  if (fstat(fd, &st) != 0) {
      perror("fstat failed");
      close(fd);
      close(socket);
      return;
  }
  uint64_t file_size = st.st_size;
  write_uint64(socket, file_size);

  ssize_t bytes;
  while ((bytes = read(fd, meow, sizeof(meow))) > 0) {
      write(socket, meow, bytes);
      memset(meow,0,sizeof(meow));
  }
  close(fd);
}

void removef(int socket){
  char path[256]={0};
  uint32_t path_size = read_uint32(socket);
  read(socket, path, path_size);
  const char *filename = strrchr(path, '/'); // getting filename from the path
  char temp[512];
  char mod_path[512];
  replace_path(path,mod_path,sizeof(mod_path));
  // Expand tilde if needed
  if (mod_path[0] == '~') {
    const char *home = getenv("HOME");
    if (!home){perror("unable to get home");}
    snprintf(temp, sizeof(temp), "%s/%s", home, mod_path + 1);
  }
  if(remove(temp)==0){printf("File %s successfully deleted from Server",filename);}

}
int compare_filenames(const void *a, const void *b) {
    const char *fa = (const char *)a;
    const char *fb = (const char *)b;
    return strcmp(fa, fb);
}


// Recursive callback for each file
int list_files_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    if (typeflag == FTW_F) {
        const char *filename = strrchr(fpath, '/');
        if (filename) filename++;
        else filename = fpath;

        if (filename[0] != '.') {
            strncpy(collected_files[file_count], filename, sizeof(collected_files[file_count]) - 1);
            file_count++;
        }
    }
    return 0; // Continue
}

void display_fnames(int socket) {
    char mod_path[256]={0};
    uint32_t path_size = read_uint32(socket);
    char path[256];
    read(socket, path, path_size);
    path[path_size] = '\0';
    replace_path(path,mod_path,sizeof(mod_path));
    printf("path: %s\n", mod_path);
    fflush(stdout);

    // Expand tilde
    char full_path[512];
    if (mod_path[0] == '~') {
        const char *home = getenv("HOME");
        snprintf(full_path, sizeof(full_path), "%s/%s", home, mod_path + 1);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", mod_path);
    }

    // Reset collection buffer
    file_count = 0;
    memset(collected_files, 0, sizeof(collected_files));

    // Walk directory recursively
    nftw(full_path, list_files_cb, 10, FTW_PHYS);

    qsort(collected_files, file_count, sizeof(collected_files[0]), compare_filenames);

    write_uint32(socket, file_count);
    for (int i = 0; i < file_count; i++) {
        write_uint32(socket, strlen(collected_files[i]));
        write(socket, collected_files[i], strlen(collected_files[i]));
        printf("[S3] Sent %s to client\n", collected_files[i]);
    }
}

void download_tarf(int socket){
    char root[256];
    char filetype[5] = {0};
    uint32_t filetype_size = read_uint32(socket);
    read(socket, filetype, filetype_size);
    const char *home = getenv("HOME");
    snprintf(root, sizeof(root), "%s/S3", home);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cd %s && find . -type f -name '*.c' | tar -cf cfiles.tar -T -", root);
    system(cmd);

    char tarpath[512];
    snprintf(tarpath, sizeof(tarpath), "%s/cfiles.tar", root);
    int fd = open(tarpath, O_RDONLY);
    if (fd < 0) {
        write_uint64(socket, 0);
        return;
    }
    struct stat st;
    fstat(fd, &st);
    write_uint64(socket, st.st_size);

    char buffer[BUFFER_SIZE];
    ssize_t r;
    while ((r = read(fd, buffer, sizeof(buffer))) > 0) {
        write(socket, buffer, r);
    }
    close(fd);
    unlink(tarpath);
}
int main(){ // Server 3 (txt Server)
    int listening_socket, connection_socket;
    socklen_t len;
    struct sockaddr_in server_address, client_address; //Socket address object to which IP address and port number are added

    //socket() sytem call
    if((listening_socket = socket(AF_INET, SOCK_STREAM, 0))<0){
        fprintf(stderr, "Could not create socket\n");
        exit(1);
    }
    //Add port number and IP address to servAdd before invoking the bind() system call
    server_address.sin_family = AF_INET;
    inet_pton(AF_INET, SERVER_IP, &server_address.sin_addr); //Add the IP address of the machine
    server_address.sin_port = htons((uint16_t)PORT);//Add the port number entered by the user
    //bind() system call
    bind(listening_socket, (struct sockaddr *) &server_address,sizeof(server_address));

    //listen
    listen(listening_socket, 10);
    //printf("[S2] Listening on port %d...\n", PORT);

    while (1){
        connection_socket= accept(listening_socket,(struct sockaddr *)&client_address,&len); //accept()
        if(connection_socket==-1){
            perror("Request Accept Failed");
        }
        char cmd[7] = {0};
        while (recv(connection_socket, cmd, 6, MSG_WAITALL) == 6) {

            if (strcmp(cmd, "uloadf") == 0) {
                uploadf(connection_socket);
            } else if (strcmp(cmd, "downlf") == 0) {
                downloadf(connection_socket);
            } else if (strcmp(cmd, "removf") == 0) {
                removef(connection_socket);
            } else if (strcmp(cmd, "dnltar") == 0) {
                download_tarf(connection_socket);
            } else if (strcmp(cmd, "dispfn") == 0) {
                display_fnames(connection_socket);
            } else {
                printf("Invalid command: %s\n", cmd);
            }
            memset(cmd, 0, sizeof(cmd));  // Clear buffer for next command
        }
        close(connection_socket);
    }
}
