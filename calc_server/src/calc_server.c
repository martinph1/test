/*
 ============================================================================
 Name        : calc_server.c
 Author      : Martin
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include "calc.pb-c.h"
#include <sys/un.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <semaphore.h>

#define SV_SOCK_PATH "/tmp/us_xfr"
#define MAX_QD_NAME_L 64
#define QD_SERVER_NAME "/calc-server"
#define MAX_MSG_SIZE 8192 + 10
#define SHMOBJ_PATH "/shmcalc"
#define SHMLOG_PATH "/shmlog"
#define MAX_SEM_NAME 64
#define SERVER_SEM_NAME "/calc-sem"
#define MAX_STRING_SIZE 256

enum _notif_type {
	CONNECTED = 0, DISCONNECTED = 1, NONE = 1,
};
typedef enum _notif_type notification;

typedef struct ListNode {
	struct ListNode *next;
	pid_t pr_id; //0 if not associated
	int fd;
	char *qd_id;
	int is_monitor;
	char *client_sem_name;
} client_id;

typedef struct List {
	int count;
	client_id *first;
} List;

typedef struct _CURRENT_CALC {
	double left_operand;
	double right_operand;
	double calc_answer;
	char oparation_sign;
} current_calc;

List *connections = NULL;

pthread_mutex_t mtx;
mqd_t qd_client, qd_server;
char* log_last_ten_lines;
char log_string[MAX_MSG_SIZE];

current_calc current_calculation;
sem_t *client_semaphore;
sem_t *server_semaphore;

void update_monitor(pid_t pr_id, int monitor, char* semaphore);

void all_connections();

void notify_all_term();

void sig_handler_exit(int signo);

void get_time(char t[]);

void remove_element_by_fd(int fd);

void remove_list();

void *handle_new_client(void* socket);

void add_client_id(int fd, pid_t pr_id, char* qd_client_name);

double calculate(double a, double b, char op);

void handle_message_queues();

void *async_work();

void server_working();

void post_to_shm(char *some_message);

void update_log();

void write_string_to_log(char*);

void send_monitors_message(char* words);

int main(void) {

	pthread_t async;
	memset(log_string,0,MAX_MSG_SIZE*sizeof(char));
	connections = malloc(sizeof(List));
	//fclose(fopen("serv.log", "w"));
	qd_server = mq_open(QD_SERVER_NAME, O_CREAT | O_RDONLY | O_NONBLOCK, 0664,
	NULL);

	signal(SIGINT, sig_handler_exit);
	signal(SIGTERM, sig_handler_exit);
	signal(SIGTSTP, sig_handler_exit);

	pthread_create(&async, NULL, async_work, NULL);
	server_working();

	remove_list();

	mq_unlink(QD_SERVER_NAME);
	shm_unlink(SHMOBJ_PATH);

	exit(EXIT_SUCCESS);
}

void sig_handler_exit(int signo) {
	printf("\n----LOL.BYE.----\n");
	sprintf(log_string, "The server is terminating\n");
	write_string_to_log(log_string);
	//send_monitors_message(current_message);
	shm_unlink(SHMOBJ_PATH);
	mq_unlink(QD_SERVER_NAME);
	notify_all_term();

	exit(EXIT_SUCCESS);
}

void *handle_new_client(void* socket) {
	Calculation *mesg;
	Calculation ans = CALCULATION__INIT;
	size_t len;
	void *s_buff = malloc(0);

	sprintf(log_string, "Established connection through socket %d",
			(int) socket);
	write_string_to_log(log_string);

	printf("\nEstablished connection with client id: %d\n",
			(int) socket);

	int n;
	uint8_t *buffer;
	double answer;
	buffer = malloc(2048);
	do {
		n = read((int) socket, buffer, 2048);
		mesg = calculation__unpack(NULL, n, buffer);
		if (!mesg)
			perror("Received empty message");

		if (n < 0) {
			perror("Couldn't read from socket");
			pthread_mutex_lock(&mtx);
			sprintf(log_string, "Couldn't read from client's socket id: %d\n",
					(int) socket);
			write_string_to_log(log_string);
			pthread_mutex_unlock(&mtx);
		}
		if (mesg->has_l_op && mesg->has_r_op) {

			pthread_mutex_lock(&mtx);
			sprintf(log_string, "Client id: %d asked to calculate %lf %s %lf",
					(int) socket, mesg->l_op, mesg->oper, mesg->r_op);
			write_string_to_log(log_string);
			pthread_mutex_unlock(&mtx);

			answer = calculate(mesg->l_op, mesg->r_op, mesg->oper[0]);
			printf("\n%lf %c %lf = %lf\n", mesg->l_op, mesg->oper[0],
					mesg->r_op, answer);
			ans.has_answer = 1;
			ans.answer = answer;
			len = calculation__get_packed_size(&ans);
			s_buff = realloc(s_buff, len);
			calculation__pack(&ans, s_buff);
			send((int) socket, s_buff, len, 0);

			pthread_mutex_lock(&mtx);

			sprintf(log_string, "%lf %s %lf = %lf is send through socket: %d",
					mesg->l_op, mesg->oper, mesg->r_op, ans.answer,
					(int) socket);
			write_string_to_log(log_string);

			pthread_mutex_unlock(&mtx);
		}
	} while (n > 0);

	all_connections();

	remove_element_by_fd((int) socket);
	close((int) socket);

	pthread_mutex_lock(&mtx);
	sprintf(log_string, "Connection through client socket: %d is closed",
			(int) socket);
	write_string_to_log(log_string);
	pthread_mutex_unlock(&mtx);
	if (buffer)
		free(buffer);
	if (s_buff)
		free(s_buff);
	return NULL;
}
void server_working() {

	int f_socket = 0, s_socket = 0;
	size_t d_count = 0;
	struct sockaddr_un serverAddr;
	pthread_t tr;
	char my_time[100];
	Calculation *f_message = NULL;
	uint8_t buffer[2048];

	f_socket = socket(AF_UNIX, SOCK_STREAM, 0);

	memset(&serverAddr, 0, sizeof(struct sockaddr_un));
	memset(&tr, 0, sizeof(pthread_t));

	if (remove(SV_SOCK_PATH) == -1 && errno != ENOENT)
		perror("\n:(\n");

	serverAddr.sun_family = AF_UNIX;
	strncpy(serverAddr.sun_path, SV_SOCK_PATH, sizeof(serverAddr.sun_path) - 1);

	bind(f_socket, (struct sockaddr *) &serverAddr, sizeof(struct sockaddr_un));

	if (listen(f_socket, 0) == 0) {
		sprintf(log_string, "%s Started listening", my_time);
		write_string_to_log(log_string);
	} else {
		sprintf(log_string, "Error occurred while trying to go live.");
		write_string_to_log(log_string);
	}

	do {
		s_socket = accept(f_socket, NULL, NULL);
		if (s_socket != 0) {
			all_connections();
			d_count = recv(s_socket, buffer, 2048, 0);
			f_message = calculation__unpack(NULL, d_count, buffer);
			if (f_message->has_pr_id && f_message->dq_cl_name) {
				add_client_id(s_socket, f_message->pr_id,
						f_message->dq_cl_name);
				tr = pthread_create(&tr, NULL, handle_new_client,
						(void*) s_socket);
			} else {
				perror("Connection with client was corrupted");
			}
		}

		if (tr > 0) {
			perror("Error on creating the thread.");
			sprintf(log_string,
					"Error occurred while trying to handle a new client.\n");
			exit(EXIT_FAILURE);
		}

	} while (1);
}

double calculate(double a, double b, char op) {
	switch (op) {
	case '+':
		current_calculation.calc_answer = a + b;
		return (a + b);
		break;
	case '-':
		current_calculation.calc_answer = a - b;
		return (a - b);
		break;
	case '/':
		current_calculation.calc_answer = a / b;
		return (a / b);
		break;
	case '*':
		current_calculation.calc_answer = a * b;
		return (a * b);
		break;
	default:
		return EIO;
		break;
	}

	return EOF;
}

void add_client_id(int fd, pid_t pr_id, char* qd_client_name) {
	client_id *cid = NULL;

	cid = malloc(sizeof(client_id));
	memset(cid,0,sizeof(client_id));
	cid->fd = fd;
	cid->pr_id = pr_id;
	if (qd_client_name) {
		cid->qd_id = malloc(strlen(qd_client_name + 1));
		strcpy(cid->qd_id, qd_client_name + 1);
	} else
		perror("Empty string passed as argument");

	cid->next = NULL;

	if (!connections->first) {
		connections->first = cid;
		connections->count = 1;
	} else {
		cid->next = connections->first;
		connections->first = cid;
		connections->count++;
	}

	pthread_mutex_lock(&mtx);
	sprintf(log_string, "Connected client with id: %d and queue name: %s",
			pr_id, qd_client_name);
	write_string_to_log(log_string);
	pthread_mutex_unlock(&mtx);

}

void remove_element_by_fd(int fd) {
	printf("\n--%s called with argumen %d\n", __func__, fd);

	client_id *current = connections->first;
	client_id *temp = NULL;

	if (connections->count == 0)
		perror("Empty list");
	else if (connections->count == 1 && connections->first->fd == fd)
		connections->first = NULL;
	else {
		while (current->next) {
			if (current->next->fd == fd) {
				temp = current->next;
				current->next = current->next->next;
				current = temp;
				break;
			}
			current = current->next;
		}
	}

	pthread_mutex_lock(&mtx);
	sprintf(log_string, "Client with id:%d has disconnected", current->pr_id);
	printf("Client pid: %d, client fd: %d has disconnected:", current->pr_id,
			current->fd);
	write_string_to_log(log_string);
	pthread_mutex_unlock(&mtx);

	if (current) {
		free(current);
		current = NULL;
		connections->count--;
	}
}

void all_connections() {
	client_id *current = connections->first;
	client_id *p = connections->first;
	printf("\n");
	while (current) {
		p = current->next;
		printf("FD: %d, PID: %d\n", current->fd, current->pr_id);
		current = p;
	}

}

void remove_list() {
	if (!connections)
		return;
	client_id *current = connections->first;
	client_id *p = connections->first;

	while (current) {
		p = current->next;
		free(current);
		current = p;
	}

	if (connections)
		free(connections);
}
void get_time(char curr_time[]) {
	struct tm *tm;
	time_t t;
	char strtime[100];
	t = time(NULL);
	tm = localtime(&t);
	strftime(strtime, sizeof(strtime), "[%d/%m/%Y %H:%M:%S]", tm);
	strcpy(curr_time, strtime);

}

void update_monitor(pid_t pr_id, int monitor, char* semaphore) {
	client_id *current = connections->first;
	client_id *p = connections->first;

	while (current) {
		p = current->next;
		if (current->pr_id == pr_id) {
			current->is_monitor = monitor;
			current->client_sem_name = malloc(MAX_SEM_NAME);
			strcpy(current->client_sem_name, semaphore);
			if (monitor) {
				printf("Client-%d is now a monitor", pr_id);
			} else {
				printf("Client-%d is no longer a monitor.", pr_id);
			}
		}
		current = p;
	}
}

void notify_all_term() {
	client_id *current = connections->first;
	client_id *p = connections->first;

	while (current) {
		p = current->next;
		if (current->pr_id)
			kill(current->pr_id, SIGUSR1);
		current = p;
	}
}
void handle_message_queues() {

	uint8_t buff[MAX_MSG_SIZE];
	Calculation *received;
	Calculation to_send = CALCULATION__INIT;
	size_t m_len;
	int flag;
	double answer;

	while (1) {
		m_len = mq_receive(qd_server, buff, MAX_MSG_SIZE, 0);
		if (m_len == -1)
			break;
		received = calculation__unpack(NULL, m_len, buff);
		if (!received)
			break;

		if (received->has_l_op && received->has_r_op && received->oper) {
			pthread_mutex_lock(&mtx);
			sprintf(log_string, "Client-%d asked to calculate %lf %s %lf = ?",
					received->pr_id, received->l_op, received->oper,
					received->r_op);
			write_string_to_log(log_string);
			pthread_mutex_unlock(&mtx);

			to_send.has_l_op = 1;
			to_send.has_r_op = 1;
			to_send.has_answer = 1;
			if (!to_send.oper) {
				to_send.oper = malloc(2);
			}
			strncpy(to_send.oper, received->oper, 2);
			to_send.l_op = received->l_op;
			to_send.r_op = received->r_op;
			answer = calculate(received->l_op, received->r_op,
					received->oper[0]);
			to_send.answer = answer;
			if (received->has_pr_id)
				to_send.pr_id = received->pr_id;
			m_len = calculation__get_packed_size(&to_send);
			calculation__pack(&to_send, buff);
			qd_client = mq_open(received->dq_cl_name,
			O_CREAT | O_WRONLY | O_NONBLOCK, 0664, NULL);
			flag = mq_send(qd_client, buff, m_len, 0);
			if (flag == -1)
				break;
			else {
				pthread_mutex_lock(&mtx);

				sprintf(log_string, "%lf %s %lf = %lf is send to Client-%d",
						to_send.l_op, to_send.oper, to_send.r_op,
						to_send.answer, to_send.pr_id);
				write_string_to_log(log_string);
				pthread_mutex_unlock(&mtx);

				printf("\nSend: %lf %s %lf = %lf\n", to_send.l_op, to_send.oper,
						to_send.r_op, to_send.answer);
			}
			mq_close(qd_client);
		} else if (received->has_monitor && received->sem_name) {
			update_monitor(received->pr_id, received->monitor,
					received->sem_name);
			printf(
					"\nClient-%d changed monitor state to: %d with semaphore name: %s\n",
					received->pr_id, received->monitor, received->sem_name);
		}
	}
}
void *async_work() {
	do {
		handle_message_queues();
	} while (1);
}
void post_to_shm(char *message) {
	int fd;
	Calculation current = CALCULATION__INIT;
	size_t len;
	uint8_t *buffer;
	uint8_t *shmem;

	fd = shm_open(SHMOBJ_PATH, O_CREAT | O_RDWR, S_IRWXU | S_IRWXG);
	if (fd < 0) {
		perror("In shm_open()");
		exit(1);
	}
	if (log_last_ten_lines) {
		current.log = malloc(strlen(log_last_ten_lines) + 1);
		strcpy(current.log, log_last_ten_lines);
	}
	if (message) {
		current.other_message = malloc(strlen(message) + 1);
		strcpy(current.other_message, message);
	}

	len = calculation__get_packed_size(&current);
	buffer = malloc(len);
	ftruncate(fd, len);
	shmem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shmem == MAP_FAILED)
		perror("Mapping failed");
	calculation__pack(&current, buffer);
	memcpy(shmem, buffer, len);

	if (buffer)
		free(buffer);
	close(fd);
}
void update_log() {

	FILE *fp;
	int count = 0;
	int w_from, w_to;
	char temp;

	fp = fopen("serv.log", "r");
	if (fp == NULL)
		return;

	fseek(fp, 0L, SEEK_SET);
	w_from = ftell(fp);

	fseek(fp, -1, SEEK_END);
	w_to = ftell(fp);

	if (w_to == w_from)
		return;
	while (count < 10) {
		fscanf(fp, "%c", &temp);
		if (temp == '\n')
			count++;
		fseek(fp, -2, SEEK_CUR);
		if (w_from == ftell(fp))
			break;

	}
	w_from = ftell(fp);

	fseek(fp, w_from - w_to, SEEK_END);

	log_last_ten_lines = malloc((w_to - w_from) + 1);
	memset(log_last_ten_lines, 0, (w_to - w_from) + 1);

	for (count = 0; (log_last_ten_lines[count] = fgetc(fp)) != EOF; count++)
		;

}

void send_monitors_message(char* words) {
	client_id *current = connections->first;
	client_id *p = connections->first;

	all_connections();

	while (current) {
		p = current->next;
		if (current->is_monitor) {
			client_semaphore = sem_open(current->client_sem_name, O_CREAT, 777,
					0);
			printf("\n%s, %s\n", __func__, current->client_sem_name);

			update_log();
			post_to_shm(words);
			sem_post(client_semaphore);
			sem_close(client_semaphore);
			sleep(1);
		}
		current = p;
	}

}
void write_string_to_log(char* log_message) {
	FILE *fp = NULL;
	char current_message[MAX_STRING_SIZE];
	char my_time[100];
	if (!log_message) {
		perror("Attempt to write empty string to file.");
		return;
	}
	fp = fopen("serv.log", "a");
	if (!fp) {
		perror("File could not be reached.");
		return;
	}
	get_time(my_time);
	sprintf(current_message, "\n%s %s", my_time, log_message);
	fprintf(fp, "%s", current_message);
	fclose(fp);
	fp = NULL;
	send_monitors_message(current_message);

}
