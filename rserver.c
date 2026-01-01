#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <argp.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <sys/poll.h>

#define MAX_CLIENTS 256
#define HEADER_SIZE 8
#define MAGIC_NUMBER 0x0417
#define SUCCESS_FLAG 0x9a00
#define ERROR_FLAG 0x9a01

typedef enum
{
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_CLOSED
} ClientState;

typedef enum
{
    CMD_CONNECT = 0x9b,
    CMD_DISCONNECT = 0xff,
    CMD_JOIN = 0x03,
    CMD_LEAVE = 0x06,
    CMD_KEEPALIVE = 0x13,
    CMD_LISTROOM = 0x09,
    CMD_LISTUSERS = 0x0c,
    CMD_NICK = 0x0f,
    CMD_PRIVATEMSG = 0x12,
    CMD_CHAT = 0x15
} Command;

typedef enum
{
    RESP_CONNECT,
    RESP_JOIN,
    RESP_JOIN_FAILED,
    RESP_LEAVE,
    RESP_NICK,
    RESP_NICK_FAILED,
    RESP_MSG,
    RESP_LIST_ROOMS,
    RESP_LIST_USERS,
    RESP_CHAT,
    RESP_CHAT_FAILED,
    RESP_MSG_FAILED
} Response;

typedef struct RoomInfo
{
    char *name;
    char *password;
    int member_count;
    struct RoomInfo *next;
} RoomInfo;

typedef struct
{
    ClientState state;
    int socket_fd;
    char nickname[256];
    RoomInfo *current_room;
} ClientInfo;

void fatal_error(const char *msg);

int accept_new_client(int server_fd, ClientInfo *client);

void process_client_message(ClientInfo *all_clients,
                            struct pollfd *poll_fds,
                            int idx);

static RoomInfo *rooms_head = NULL;
static const int MAX_PENDING_CONNECTIONS = 5;

struct ServerConfig
{
    int port;
};

int setup_server_socket(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        fatal_error("socket fail");
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        fatal_error("setsockopt fail");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fatal_error("bind fail");
    }

    if (listen(sock, MAX_PENDING_CONNECTIONS) < 0)
    {
        fatal_error("listen fail");
    }

    return sock;
}

void handle_new_connection(int server_sock, ClientInfo *clients, struct pollfd *fds)
{
    int i = 0;
    while (i < MAX_CLIENTS)
    {
        if (fds[i + 1].fd < 0)
        {
            fds[i + 1].fd = accept_new_client(server_sock, &clients[i]);
            fds[i + 1].events = POLLIN;
            break;
        }
        i++;
    }
}

void handle_client_events(ClientInfo *clients, struct pollfd *fds, int count)
{
    int i = 0;
    while (i < count)
    {
        if (fds[i + 1].fd >= 0 && (fds[i + 1].revents & POLLIN))
        {
            process_client_message(clients, fds, i);
        }
        i++;
    }
}

error_t parse_arguments(int key, char *arg, struct argp_state *state)
{
    struct ServerConfig *config = state->input;

    if (key == 'p')
    {
        config->port = atoi(arg);
        if (config->port <= 0 || config->port > 65535)
        {
            argp_error(state, "Invalid option for a port, must be a number between 1-65535");
        }
        return 0;
    }

    return ARGP_ERR_UNKNOWN;
}

void initialize_server_config(struct ServerConfig *config, int argc, char *argv[])
{
    bzero(config, sizeof(*config));

    struct argp_option options[] = {
        {"port", 'p', "port", 0, "The port to be used for the server", 0},
        {0}};

    struct argp argp_settings = {options, parse_arguments, 0, 0, 0, 0, 0};

    if (argp_parse(&argp_settings, argc, argv, 0, NULL, config) != 0)
    {
        fprintf(stderr, "Got an error condition when parsing\n");
    }

    if (!config->port)
    {
        fputs("A port number must be specified with -p\n", stderr);
        exit(1);
    }
}

void fatal_error(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

int receive_exact_bytes(int fd, void *buf, int num_bytes)
{
    int total_received = 0;
    int result;

    while (total_received < num_bytes)
    {
        result = recv(fd, (char *)buf + total_received, num_bytes - total_received, 0);

        if (result == -1)
        {
            return -1;
        }

        if (result == 0)
        {
            return 0;
        }

        total_received += result;
    }

    return total_received;
}

void transmit_bytes(int fd, void *buf, int num_bytes)
{
    int total_sent = 0;
    int result;

    while (total_sent < num_bytes)
    {
        result = send(fd, (char *)buf + total_sent, num_bytes - total_sent, 0);

        if (result == -1)
        {
            break;
        }

        total_sent += result;
    }
}

void transmit_error_message(ClientInfo *client, char *error_text)
{
    int text_length = strlen(error_text);
    uint32_t payload_length = text_length + 1;
    int buffer_length = HEADER_SIZE + text_length;

    uint8_t *packet = malloc(buffer_length);
    memset(packet, 0, buffer_length);

    uint32_t net_payload_len = htonl(payload_length);
    uint16_t net_magic = htons(MAGIC_NUMBER);
    uint16_t net_flag = htons(ERROR_FLAG);

    memcpy(packet, &net_payload_len, 4);
    memcpy(packet + 4, &net_magic, 2);
    memcpy(packet + 6, &net_flag, 2);
    memcpy(packet + 8, error_text, text_length);

    transmit_bytes(client->socket_fd, packet, buffer_length);
    free(packet);
}

RoomInfo *find_room(char *room_name)
{
    RoomInfo *curr = rooms_head;

    while (curr)
    {
        if (strcmp(curr->name, room_name) == 0)
        {
            return curr;
        }
        curr = curr->next;
    }

    return NULL;
}

ClientInfo *find_client_by_nickname(ClientInfo *clients, char *nick)
{
    int i = 0;

    while (i < MAX_CLIENTS)
    {
        if (clients[i].state == STATE_CONNECTED && strcmp(clients[i].nickname, nick) == 0)
        {
            return &clients[i];
        }
        i++;
    }

    return NULL;
}

void dispatch_private_message(ClientInfo *sender, ClientInfo *recipient, char *text)
{
    uint8_t sender_nick_len = strlen(sender->nickname);
    int text_len = strlen(text);
    uint16_t net_text_len = htons(text_len);

    uint32_t payload_size = 1 + sender_nick_len + 2 + text_len;
    int total_size = 7 + payload_size;

    uint8_t *packet = malloc(total_size);
    memset(packet, 0, total_size);

    uint32_t net_payload_size = htonl(payload_size);
    uint16_t net_magic = htons(MAGIC_NUMBER);
    uint8_t message_flag = 0x12;

    int offset = 0;
    memcpy(packet + offset, &net_payload_size, 4);
    offset += 4;
    memcpy(packet + offset, &net_magic, 2);
    offset += 2;
    memcpy(packet + offset, &message_flag, 1);
    offset += 1;
    memcpy(packet + offset, &sender_nick_len, 1);
    offset += 1;
    memcpy(packet + offset, sender->nickname, sender_nick_len);
    offset += sender_nick_len;
    memcpy(packet + offset, &net_text_len, 2);
    offset += 2;
    memcpy(packet + offset, text, text_len);

    transmit_bytes(recipient->socket_fd, packet, total_size);
    free(packet);
}

void broadcast_to_room(ClientInfo *sender, RoomInfo *room, ClientInfo *all_clients, char *text)
{
    uint8_t room_name_len = strlen(room->name);
    uint8_t sender_nick_len = strlen(sender->nickname);
    int text_len = strlen(text);
    uint16_t net_text_len = htons(text_len);

    uint32_t payload_size = 1 + room_name_len + 1 + sender_nick_len + 2 + text_len;
    int total_size = 7 + payload_size;

    uint8_t *packet = malloc(total_size);
    memset(packet, 0, total_size);

    uint32_t net_payload_size = htonl(payload_size);
    uint16_t net_magic = htons(MAGIC_NUMBER);
    uint8_t chat_flag = 0x15;

    int offset = 0;
    memcpy(packet + offset, &net_payload_size, 4);
    offset += 4;
    memcpy(packet + offset, &net_magic, 2);
    offset += 2;
    memcpy(packet + offset, &chat_flag, 1);
    offset += 1;
    memcpy(packet + offset, &room_name_len, 1);
    offset += 1;
    memcpy(packet + offset, room->name, room_name_len);
    offset += room_name_len;
    memcpy(packet + offset, &sender_nick_len, 1);
    offset += 1;
    memcpy(packet + offset, sender->nickname, sender_nick_len);
    offset += sender_nick_len;
    memcpy(packet + offset, &net_text_len, 2);
    offset += 2;
    memcpy(packet + offset, text, text_len);

    int idx = 0;
    while (idx < MAX_CLIENTS)
    {
        if (&all_clients[idx] != sender &&
            all_clients[idx].current_room == room &&
            all_clients[idx].state == STATE_CONNECTED)
        {
            transmit_bytes(all_clients[idx].socket_fd, packet, total_size);
        }
        idx++;
    }

    free(packet);
}

int string_comparator(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

void send_response(int fd, ClientInfo *all_clients, int client_idx, int response_type)
{
    ClientInfo *client = &all_clients[client_idx];
    uint8_t *buffer = NULL;
    int buffer_size;
    uint32_t payload_len;
    uint16_t net_magic = htons(MAGIC_NUMBER);
    uint16_t net_flag = htons(SUCCESS_FLAG);

    if (response_type == RESP_CONNECT)
    {
        snprintf(client->nickname, sizeof(client->nickname), "rand%d", client_idx);
        int nick_len = strlen(client->nickname);
        buffer_size = HEADER_SIZE + nick_len;
        uint32_t connect_payload_len = nick_len + 1;

        buffer = malloc(buffer_size);
        memset(buffer, 0, buffer_size);

        connect_payload_len = htonl(connect_payload_len);
        memcpy(buffer, &connect_payload_len, 4);
        memcpy(buffer + 4, &net_magic, 2);
        memcpy(buffer + 6, &net_flag, 2);
        memcpy(buffer + 8, client->nickname, nick_len);

        transmit_bytes(fd, buffer, buffer_size);
        free(buffer);
        client->state = STATE_CONNECTED;
    }
    else if (response_type == RESP_JOIN || response_type == RESP_LEAVE ||
             response_type == RESP_NICK || response_type == RESP_CHAT ||
             response_type == RESP_MSG)
    {
        buffer_size = HEADER_SIZE;
        buffer = malloc(buffer_size);
        memset(buffer, 0, buffer_size);

        payload_len = htonl(1);
        memcpy(buffer, &payload_len, 4);
        memcpy(buffer + 4, &net_magic, 2);
        memcpy(buffer + 6, &net_flag, 2);

        transmit_bytes(fd, buffer, buffer_size);
        free(buffer);
    }
    else if (response_type == RESP_JOIN_FAILED)
    {
        transmit_error_message(client, "Incorrect password. Maybe try 'Alohomora'?");
    }
    else if (response_type == RESP_LIST_ROOMS)
    {
        int room_count = 0;
        RoomInfo *curr = rooms_head;

        while (curr)
        {
            room_count++;
            curr = curr->next;
        }

        char **room_names = malloc(room_count * sizeof(char *));
        curr = rooms_head;
        int idx = 0;

        while (curr)
        {
            room_names[idx++] = curr->name;
            curr = curr->next;
        }

        qsort(room_names, room_count, sizeof(char *), string_comparator);

        int list_size = 0;
        idx = 0;
        while (idx < room_count)
        {
            list_size += 1 + strlen(room_names[idx]);
            idx++;
        }

        buffer_size = HEADER_SIZE + list_size;
        buffer = malloc(buffer_size);
        memset(buffer, 0, buffer_size);

        payload_len = htonl(list_size + 1);
        memcpy(buffer, &payload_len, 4);
        memcpy(buffer + 4, &net_magic, 2);
        memcpy(buffer + 6, &net_flag, 2);

        int pos = HEADER_SIZE;
        idx = 0;
        while (idx < room_count)
        {
            uint8_t name_len = strlen(room_names[idx]);
            memcpy(buffer + pos, &name_len, 1);
            memcpy(buffer + pos + 1, room_names[idx], name_len);
            pos += 1 + name_len;
            idx++;
        }

        transmit_bytes(fd, buffer, buffer_size);
        free(buffer);
        free(room_names);
    }
    else if (response_type == RESP_LIST_USERS)
    {
        // Count matching users
        int user_count = 0;
        int i = 0;

        while (i < MAX_CLIENTS)
        {
            int in_same_room = (client->current_room && all_clients[i].current_room == client->current_room);
            int connected_no_room = (!client->current_room && all_clients[i].state == STATE_CONNECTED);

            if (in_same_room || connected_no_room)
            {
                user_count++;
            }
            i++;
        }

        // Collect nicknames
        char **nicknames = malloc(user_count * sizeof(char *));
        i = 0;
        int idx = 0;

        while (i < MAX_CLIENTS)
        {
            int in_same_room = (client->current_room && all_clients[i].current_room == client->current_room);
            int connected_no_room = (!client->current_room && all_clients[i].state == STATE_CONNECTED);

            if (in_same_room || connected_no_room)
            {
                nicknames[idx++] = all_clients[i].nickname;
            }
            i++;
        }

        // Sort alphabetically
        qsort(nicknames, user_count, sizeof(char *), string_comparator);

        // Calculate total size
        int list_size = 0;
        idx = 0;
        while (idx < user_count)
        {
            list_size += 1 + strlen(nicknames[idx]);
            idx++;
        }

        buffer_size = HEADER_SIZE + list_size;
        buffer = malloc(buffer_size);
        memset(buffer, 0, buffer_size);

        payload_len = htonl(list_size + 1);
        memcpy(buffer, &payload_len, 4);
        memcpy(buffer + 4, &net_magic, 2);
        memcpy(buffer + 6, &net_flag, 2);

        int pos = HEADER_SIZE;
        idx = 0;

        while (idx < user_count)
        {
            uint8_t nick_len = strlen(nicknames[idx]);
            memcpy(buffer + pos, &nick_len, 1);
            memcpy(buffer + pos + 1, nicknames[idx], nick_len);
            pos += 1 + nick_len;
            idx++;
        }

        transmit_bytes(fd, buffer, buffer_size);
        free(buffer);
        free(nicknames);
    }
    else if (response_type == RESP_CHAT_FAILED)
    {
        transmit_error_message(client, "You're talking to the walls. No one is here to listen.");
    }
    else if (response_type == RESP_NICK_FAILED)
    {
        transmit_error_message(client, "That name's already on the Marauder's Map. Choose another.");
    }
    else if (response_type == RESP_MSG_FAILED)
    {
        transmit_error_message(client, "That wizard isn't here. Maybe try the Room of Requirement?");
    }
}

int accept_new_client(int server_socket, ClientInfo *client)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int new_fd = accept(server_socket, (struct sockaddr *)&client_addr, &addr_len);

    if (new_fd == -1)
    {
        fatal_error("server errorrrr");
    }

    memset(client, 0, sizeof(*client));
    client->socket_fd = new_fd;
    client->state = STATE_CONNECTING;

    return new_fd;
}

void process_client_message(ClientInfo *all_clients, struct pollfd *poll_fds, int idx)
{
    ClientInfo *client = &all_clients[idx];
    int fd = client->socket_fd;
    uint8_t cmd = 0;

    uint32_t payload_size = 0;
    int bytes_read = receive_exact_bytes(fd, &payload_size, 4);

    if (bytes_read == 0)
    {
        cmd = CMD_DISCONNECT;
    }
    else
    {
        payload_size = ntohl(payload_size);
        uint16_t magic = 0;
        bytes_read = receive_exact_bytes(fd, &magic, 2);

        if (bytes_read == 0)
        {
            cmd = CMD_DISCONNECT;
        }
        else
        {
            magic = ntohs(magic);
            bytes_read = receive_exact_bytes(fd, &cmd, 1);
            cmd = (bytes_read == 0) ? CMD_DISCONNECT : cmd;
        }
    }

    if (cmd == CMD_CONNECT)
    {
        uint8_t *payload = malloc(payload_size);
        receive_exact_bytes(fd, payload, payload_size);
        free(payload);

        send_response(fd, all_clients, idx, RESP_CONNECT);
    }
    else if (cmd == CMD_KEEPALIVE)
    {
        uint8_t *payload = malloc(payload_size);
        receive_exact_bytes(fd, payload, payload_size);
        free(payload);
    }
    else if (cmd == CMD_DISCONNECT)
    {
        close(client->socket_fd);
        client->socket_fd = -1;
        poll_fds[idx + 1].fd = -1;
        memset(client->nickname, 0, sizeof(client->nickname));
        client->current_room = NULL;
        client->state = STATE_CLOSED;
    }
    else if (cmd == CMD_JOIN)
    {
        uint8_t room_name_len = 0;
        receive_exact_bytes(fd, &room_name_len, 1);

        uint8_t *room_name = malloc(room_name_len + 1);
        receive_exact_bytes(fd, room_name, room_name_len);
        room_name[room_name_len] = '\0';

        uint8_t pwd_len = 0;
        receive_exact_bytes(fd, &pwd_len, 1);

        uint8_t *pwd = NULL;
        if (pwd_len > 0)
        {
            pwd = malloc(pwd_len + 1);
            receive_exact_bytes(fd, pwd, pwd_len);
            pwd[pwd_len] = '\0';
        }

        RoomInfo *room = find_room((char *)room_name);

        if (!room)
        {
            room = malloc(sizeof(RoomInfo));
            memset(room, 0, sizeof(RoomInfo));

            room->name = malloc(room_name_len + 1);
            strcpy(room->name, (char *)room_name);
            room->name[room_name_len] = '\0';

            if (pwd_len > 0)
            {
                room->password = malloc(pwd_len + 1);
                strcpy(room->password, (char *)pwd);
                room->password[pwd_len] = '\0';
            }
            else
            {
                room->password = NULL;
            }

            room->member_count = 1;
            room->next = rooms_head;
            rooms_head = room;

            client->current_room = room;
            send_response(fd, all_clients, idx, RESP_JOIN);
        }
        else
        {
            int pwd_match = (room->password && pwd && strcmp(room->password, (char *)pwd) == 0) ||
                            (!room->password && !pwd);

            if (pwd_match)
            {
                // If client is already in a different room, leave it first
                if (client->current_room && client->current_room != room)
                {
                    client->current_room->member_count--;
                    if (client->current_room->member_count == 0)
                    {
                        RoomInfo *curr = rooms_head;
                        RoomInfo *prev = NULL;
                        while (curr)
                        {
                            if (curr == client->current_room)
                            {
                                prev ? (prev->next = curr->next) : (rooms_head = curr->next);
                                free(curr->name);
                                if (curr->password)
                                    free(curr->password);
                                free(curr);
                                break;
                            }
                            prev = curr;
                            curr = curr->next;
                        }
                    }
                }

                // Only increment if not already in this room
                if (client->current_room != room)
                {
                    room->member_count++;
                }

                client->current_room = room;
                send_response(fd, all_clients, idx, RESP_JOIN);
            }
            else
            {
                send_response(fd, all_clients, idx, RESP_JOIN_FAILED);
            }
        }

        free(room_name);
        if (pwd)
        {
            free(pwd);
        }
    }
    else if (cmd == CMD_LEAVE)
    {
        if (client->current_room)
        {
            RoomInfo *room = client->current_room;
            room->member_count--;

            if (room->member_count == 0)
            {
                RoomInfo *curr = rooms_head;
                RoomInfo *prev = NULL;

                while (curr)
                {
                    if (curr == room)
                    {
                        prev ? (prev->next = curr->next) : (rooms_head = curr->next);

                        free(curr->name);
                        if (curr->password)
                        {
                            free(curr->password);
                        }
                        free(curr);
                        break;
                    }
                    prev = curr;
                    curr = curr->next;
                }
            }

            client->current_room = NULL;
            send_response(fd, all_clients, idx, RESP_LEAVE);
        }
        else
        {
            close(client->socket_fd);
            client->socket_fd = -1;
            poll_fds[idx + 1].fd = -1;
            memset(client->nickname, 0, sizeof(client->nickname));
            client->current_room = NULL;
            client->state = STATE_CLOSED;
        }
    }
    else if (cmd == CMD_LISTROOM)
    {
        send_response(fd, all_clients, idx, RESP_LIST_ROOMS);
    }
    else if (cmd == CMD_LISTUSERS)
    {
        send_response(fd, all_clients, idx, RESP_LIST_USERS);
    }
    else if (cmd == CMD_NICK)
    {
        uint8_t nick_len = 0;
        receive_exact_bytes(fd, &nick_len, 1);

        uint8_t *new_nick = malloc(nick_len + 1);
        receive_exact_bytes(fd, new_nick, nick_len);
        new_nick[nick_len] = '\0';

        ClientInfo *existing = find_client_by_nickname(all_clients, (char *)new_nick);

        if (existing && existing != client)
        {
            send_response(fd, all_clients, idx, RESP_NICK_FAILED);
        }
        else
        {
            strncpy(client->nickname, (char *)new_nick, sizeof(client->nickname) - 1);
            client->nickname[sizeof(client->nickname) - 1] = '\0';
            send_response(fd, all_clients, idx, RESP_NICK);
        }

        free(new_nick);
    }
    else if (cmd == CMD_PRIVATEMSG)
    {
        uint8_t target_nick_len = 0;
        receive_exact_bytes(fd, &target_nick_len, 1);

        uint8_t *target_nick = malloc(target_nick_len + 1);
        receive_exact_bytes(fd, target_nick, target_nick_len);
        target_nick[target_nick_len] = '\0';

        uint16_t msg_len = 0;
        receive_exact_bytes(fd, &msg_len, 2);
        msg_len = ntohs(msg_len);

        uint8_t *message = malloc(msg_len + 1);
        receive_exact_bytes(fd, message, msg_len);
        message[msg_len] = '\0';

        ClientInfo *recipient = find_client_by_nickname(all_clients, (char *)target_nick);

        if (!recipient || recipient == client)
        {
            send_response(fd, all_clients, idx, RESP_MSG_FAILED);
        }
        else
        {
            dispatch_private_message(client, recipient, (char *)message);
            dispatch_private_message(client, client, (char *)message);
            send_response(fd, all_clients, idx, RESP_MSG);
        }

        free(target_nick);
        free(message);
    }
    else if (cmd == CMD_CHAT)
    {
        uint8_t room_len = 0;
        receive_exact_bytes(fd, &room_len, 1);

        uint8_t *room_name = NULL;
        if (room_len > 0)
        {
            room_name = malloc(room_len + 1);
            receive_exact_bytes(fd, room_name, room_len);
            room_name[room_len] = '\0';
        }

        uint16_t msg_len = 0;
        receive_exact_bytes(fd, &msg_len, 2);
        msg_len = ntohs(msg_len);

        uint8_t *message = malloc(msg_len + 1);
        receive_exact_bytes(fd, message, msg_len);
        message[msg_len] = '\0';

        if (room_len == 0 || !client->current_room)
        {
            send_response(fd, all_clients, idx, RESP_CHAT_FAILED);
        }
        else
        {
            RoomInfo *room = find_room((char *)room_name);

            if (!room)
            {
                send_response(fd, all_clients, idx, RESP_CHAT_FAILED);
            }
            else
            {
                broadcast_to_room(client, room, all_clients, (char *)message);
                send_response(fd, all_clients, idx, RESP_CHAT);
            }
        }

        if (room_name)
            free(room_name);
        free(message);
    }
}

int main(int argc, char *argv[])
{
    struct ServerConfig config;
    initialize_server_config(&config, argc, argv);

    ClientInfo clients[MAX_CLIENTS] = {0};
    struct pollfd poll_fds[MAX_CLIENTS + 1] = {0};

    int server_socket = setup_server_socket(config.port);

    poll_fds[0].fd = server_socket;
    poll_fds[0].events = POLLIN;

    // Initialize client poll descriptors
    int i = 0;
    while (i < MAX_CLIENTS)
    {
        poll_fds[i + 1].fd = -1;
        i++;
    }

    int ready;
    int has_new_connection;

    while (1)
    {
        ready = poll(poll_fds, MAX_CLIENTS + 1, -1);

        if (ready < 0)
        {
            fatal_error("poll fail");
        }

        has_new_connection = (poll_fds[0].revents & POLLIN);

        // Handle new connections
        if (has_new_connection)
        {
            handle_new_connection(server_socket, clients, poll_fds);
        }

        // Handle client events
        handle_client_events(clients, poll_fds, MAX_CLIENTS);
    }

    close(server_socket);
    return 0;
}
