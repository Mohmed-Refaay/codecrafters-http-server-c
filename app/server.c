#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <zconf.h>
#include <zlib.h>

struct request_details {
  char method[16];
  char path[256];
};

struct request_details get_http_details(char **buffer_ptr);
int getArgValueIndex(char *arg, int argc, char *argv[]);
int match_request(struct request_details request, char *method, char *path);
int run_server();
char *get_header_value(char *headers, char *header_name);
char *gzip_string(char *str, int *len);

int main(int argc, char *argv[]) {
  // Disable output buffering
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  int directory_index = getArgValueIndex("--directory", argc, argv);
  if (directory_index == -1) {
    printf("Usage: ./server --directory <path>\n");
  }
  char *directory = argv[directory_index];

  // You can use print statements as follows for debugging, they'll be visible
  int server_fd = run_server();
  printf("Waiting for a client to connect...\n");

  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);

  // Accepting a new connection
  while (1) {
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
                           (socklen_t *)&client_addr_len);

    printf("Client connected\n");

    if (client_fd == -1) {
      printf("Accept failed: %s \n", strerror(errno));
      return 1;
    }

    // Create a new process to handle the client
    int pid = fork();
    if (pid == -1) {
      printf("Fork failed: %s \n", strerror(errno));
      close(client_fd);
      continue;
    }

    // Parent process should close the client_fd and continue accepting new
    if (pid > 0) {
      close(client_fd);
      continue;
    }

    char buffer[1028];
    read(client_fd, buffer, sizeof(buffer));
    printf("%s", buffer);

    char *headers = buffer;

    char *accepted_encoding = get_header_value(headers, "Accept-Encoding");

    struct request_details client_request = get_http_details(&headers);

    char response[1028];
    if (match_request(client_request, "GET", "/")) {
      sprintf(response, "HTTP/1.1 200 OK\r\n\r\n");
    } else if (match_request(client_request, "GET", "/echo/")) {
      char echo_val[128];
      sscanf(client_request.path, "/echo/%s", echo_val);

      if (strstr(accepted_encoding, "gzip") != NULL) {
        char encodingHeader[128];
        int len = 0;
        char *echo_ptr;

        echo_ptr = gzip_string(echo_ptr, &len);
        sprintf(encodingHeader, "Content-Encoding: gzip\r\n");
        printf("Compressed string: %s\n", echo_ptr);
        sprintf(
            response,
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n%sContent-Length: "
            "%d\r\n\r\n%s",
            encodingHeader, len, echo_ptr);
        free(echo_ptr);
      } else {
        sprintf(
            response,
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
            "%d\r\n\r\n%s",
            (int)strlen(echo_val), echo_val);
      }

    } else if (match_request(client_request, "GET", "/user-agent")) {

      char *user_agent = get_header_value(headers, "User-Agent");

      sprintf(response,
              "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
              "%d\r\n\r\n%s",
              (int)strlen(user_agent), user_agent);

      free(user_agent);
    } else if (match_request(client_request, "GET", "/files/")) {
      char *request_path = client_request.path;

      char *file = strchr(request_path + 1, '/');

      char *path = strcat(directory, file + 1);

      printf("path: %s\n", path);

      int file_fd = open(path, O_RDONLY);

      if (file_fd == -1) {
        sprintf(response, "HTTP/1.1 404 Not Found\r\n\r\n");
      } else {
        char buffer[2056];
        int read_bytes = read(file_fd, buffer, sizeof(buffer));

        sprintf(response,
                "HTTP/1.1 200 OK\r\nContent-Type: "
                "application/octet-stream\r\nContent-Length: %d\r\n\r\n%s",
                read_bytes, buffer);
      }
    } else if (match_request(client_request, "POST", "/files/")) {
      char *request_path = client_request.path;

      char *file = strchr(request_path + 1, '/');

      char *path = strcat(directory, file + 1);

      int file_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC);

      if (file_fd == -1) {
        sprintf(response, "HTTP/1.1 404 Not Found\r\n\r\n");
      } else {
        char *body = headers;
        while (strncmp(body, "\r\n\r\n", strlen("\r\n\r\n"))) {
          body++;
        }
        body += strlen("\r\n\r\n");

        write(file_fd, body, strlen(body));

        sprintf(response, "HTTP/1.1 201 Created\r\n\r\n");
      }

      printf("POST request\n");
    } else {
      sprintf(response, "HTTP/1.1 404 Not Found\r\n\r\n");
    }

    if (write(client_fd, response, strlen(response)) == -1) {
      printf("Write failed: %s \n", strerror(errno));
    }

    free(accepted_encoding);
    close(client_fd);
  }

  close(server_fd);
  return 0;
}

struct request_details get_http_details(char **buffer_ptr) {
  struct request_details details;

  char *first_line = strsep(buffer_ptr, "\r\n");

  sscanf(first_line, "%s %s", details.method, details.path);

  return details;
}

int getArgValueIndex(char *arg, int argc, char *argv[]) {
  for (int i = 0; i < argc; i++) {
    if (!strcmp(arg, argv[i])) {
      if (i + 1 >= argc) {
        return -1;
      } else {
        return i + 1;
      }
    }
  }
  return -1;
}

int run_server() {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    printf("Socket creation failed: %s...\n", strerror(errno));
    exit(1);
  }

  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    printf("SO_REUSEADDR failed: %s \n", strerror(errno));
    exit(1);
  }

  struct sockaddr_in serv_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(4221),
      .sin_addr = {htonl(INADDR_ANY)},
  };

  if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
    printf("Bind failed: %s \n", strerror(errno));
    exit(1);
  }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    printf("Listen failed: %s \n", strerror(errno));
    exit(1);
  }

  return server_fd;
}

char *get_header_value(char *headers, char *header_name) {
  char *header_copy = strdup(headers);
  if (header_copy == NULL) {
    printf("strdup failed: %s \n", strerror(errno));
    return NULL;
  }

  char *ptr = header_copy;

  while (strncmp(ptr, header_name, strlen(header_name))) {
    ptr++;

    if (*ptr == '\0') {
      free(header_copy);
      return NULL;
    }
  }

  ptr = strstr(ptr, ": ") + 2;
  if (ptr == NULL) {
    free(header_copy);

    return NULL;
  }
  char *end = strstr(ptr, "\r\n");
  if (end == NULL) {
    free(header_copy);
  }
  *end = '\0';

  char *value = strdup(ptr);

  free(header_copy);

  return value;
}

int match_request(struct request_details request, char *method, char *path) {
  char *ptr = path + 1;
  ptr = strstr(ptr, "/");

  if (ptr == NULL) {
    return !strcmp(request.method, method) && !strcmp(request.path, path);
  }

  return !strcmp(request.method, method) &&
         !strncmp(request.path, path, strlen(path));
}

char *gzip_string(char *str, int *len) {
  int input_len = strlen(str);

  int max_output_len = compressBound(input_len);
  char *compressed_str = malloc(max_output_len);

  if (compressed_str == NULL) {
    printf("malloc failed: %s \n", strerror(errno));
    return NULL;
  }

  int result = compress2((Bytef *)compressed_str, (uLongf *)&max_output_len,
                         (Bytef *)str, input_len, Z_BEST_COMPRESSION);

  if (result != Z_OK) {
    printf("Compression failed: %s \n", strerror(errno));
    free(compressed_str);
    return NULL;
  }

  *len = max_output_len;

  return compressed_str;
}
