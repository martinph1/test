/*
 ============================================================================
 Name        : cacl_client.c
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
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "calc.pb-c.h"
#include <sys/un.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>

#define MAX_MSG_SIZE 2048
#define SV_SOCK_PATH "/tmp/us_xfr"
#define MAX_QD_NAME_L 64
#define QD_SERVER_NAME "/calc-server"
#define MAX_QUEUE_MSG_SIZE 1024
#define SHMOBJ_PATH "/shmcalc"
#define SHMLOG "/shmlog"
#define MAX_SEM_NAME 64
#define SERVER_SEM_NAME "/calc-sem"

enum client_mode {
	MONITOR = 0, NORMAL = 1,
};

// global variables, use with caution!!!
int sender, id;
struct sockaddr_un serverAddr;
mqd_t qd_client, qd_server;
char qd_client_name[MAX_QD_NAME_L];
char client_sem_name[MAX_SEM_NAME];
sem_t *semaphore;

void call_sync(double l, double r, char* s);
void call_async(double l, double r, char* s);
void server_signal(void* arg);
void server_term(void *arg);
void receive_queued(enum client_mode t);
void *listener();
void post_monitoring_state(protobuf_c_boolean state);
void read_log();

static void init_comunication() {
	void *buff;

	Calculation to_send = CALCULATION__INIT;
	size_t len;

	sender = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sender == -1) {
		perror("Cant open socket at 59 line");
	}
	serverAddr.sun_family = AF_UNIX;
	strncpy(serverAddr.sun_path, SV_SOCK_PATH, sizeof(serverAddr.sun_path) - 1);

	do {
		id = connect(sender, (struct sockaddr*) &serverAddr,
				sizeof(serverAddr));
		if (id < 0) {
			printf(
					"\nServer not found.Trying to reconect.... (Press ctrl + c to exit)");

		}
		sleep(2);
	} while (id < 0);

	sprintf(qd_client_name, "/calc-client-%d", getpid());
	to_send.has_pr_id = 1;
	to_send.pr_id = getpid();
	to_send.dq_cl_name = malloc(MAX_QD_NAME_L);
	strncpy(to_send.dq_cl_name, qd_client_name, MAX_QD_NAME_L);

	len = calculation__get_packed_size(&to_send);
	buff = malloc(len);
	calculation__pack(&to_send, buff);
	if (id == 0) {
		write(sender, buff, len);
	}
	free(buff);
}

static void init_mq_communication() {
	struct mq_attr attr;
	attr.mq_flags = 0;
	attr.mq_maxmsg = 50;
	attr.mq_msgsize = 512;

	qd_client = mq_open(qd_client_name, O_CREAT | O_RDONLY | O_NONBLOCK, 0664,
			&attr);

	mq_getattr(qd_client, &attr);
	if (qd_client == -1)
		perror("Can't open the queue!");
}

int main(int argc, char* argv[]) {
	double left;
	double right;

	char qd_client_name[MAX_QD_NAME_L];
	char* s_oper = malloc(2 * sizeof(char));

	int ans = 0;
	int method = 0;

	sprintf(client_sem_name, "/calc-sem-client-%d", getpid());
	//int fd = fileno(stdin);

	//char shall_exit;
	//char buf[10];

	pthread_t as_wait;

	signal(SIGINT, server_signal);
	signal(SIGTERM, server_signal);
	signal(SIGTSTP, server_signal);
	signal(SIGUSR1, server_term);

	pthread_create(&as_wait, NULL, listener, NULL);

	init_comunication();
	init_mq_communication();

	//set_non_blocking(fd);
	do {
		printf("\nChoose option (0-Calculator, 2-Monitor) : ");
		scanf("%d", &method);
		if (method == 0) {
			printf("\nChoose option (0-Sync, 1-Async): ");
			scanf("%d", &ans);
			printf("\nEnter left operand: ");
			scanf("%lf", &left);
			printf("\nEnter operation: ");
			scanf("%s", s_oper);
			printf("\nEnter right operand: ");
			scanf("%lf", &right);
			if (ans == 0)
				call_sync(left, right, s_oper);
			else if (ans == 1)
				call_async(left, right, s_oper);
		} else if (method == 1) {
			printf("\nEntering monitor mode. Press any key to exit.\n");
			post_monitoring_state(1);
			semaphore = sem_open(client_sem_name, O_CREAT, 777, 0);
			if (semaphore == -1) {
				perror("Semaphore could not be opened.");
			}
			while (1) {
				sem_wait(semaphore);
				read_log();
			}
			post_monitoring_state(0);
		}

	} while (1);

	close(sender);
	mq_unlink(qd_client_name);
	shm_unlink(SHMOBJ_PATH);
	mq_close(qd_client);
	exit(EXIT_SUCCESS);
}

void call_sync(double l_oper, double r_oper, char* operat) {

	void *buff;
	uint8_t *r_buff;
	size_t len;
	int b_count;
	Calculation to_send = CALCULATION__INIT;
	Calculation *reciv;

	r_buff = malloc(MAX_MSG_SIZE);

	to_send.has_l_op = 1;
	to_send.l_op = l_oper;
	to_send.has_r_op = 1;
	to_send.r_op = r_oper;
	to_send.oper = operat;

	len = calculation__get_packed_size(&to_send);
	buff = malloc(len);
	calculation__pack(&to_send, buff);

	if (id == 0) {
		write(sender, buff, len);
		printf("Connection..%d..\n", len);
		do {
			b_count = recv(sender, r_buff, MAX_MSG_SIZE, 0);
			reciv = calculation__unpack(NULL, b_count, r_buff);
			if (reciv->has_answer) {
				printf("%lf %c %lf = %lf\n", l_oper, operat[0], r_oper,
						reciv->answer);
				break;
			}

		} while (b_count > 0);
	} else {
		printf("Connection failure. Server not responding..\n");
		exit(EXIT_FAILURE);
	}

	free(buff);

}
void server_signal(void* arg) {

	char mq_name[MAX_QD_NAME_L];
	sprintf(mq_name, "/calc-client-%d", getpid());
	printf("\n\n---May the odds be ever in your favor----\n\n");
	mq_close(qd_client);
	mq_unlink(mq_name);
	shm_unlink(SHMOBJ_PATH);
	close(sender);

	exit(EXIT_SUCCESS);
}

void server_term(void *arg) {
	printf("\nServer terminating...");
	kill(getpid(), SIGINT);
}
void call_async(double l, double r, char* s) {

	void *buff;
	Calculation to_send = CALCULATION__INIT;
	size_t m_len;
	int flag;

	qd_server = mq_open(QD_SERVER_NAME, O_CREAT | O_WRONLY | O_NONBLOCK, 0664,
	NULL);
	if (qd_server == -1)
		perror("Couldn't open server's message queue");

	to_send.has_l_op = 1;
	to_send.has_r_op = 1;
	to_send.oper = malloc(2 * sizeof(char));
	to_send.dq_cl_name = malloc(MAX_QD_NAME_L);
	sprintf(to_send.dq_cl_name, "/calc-client-%d", getpid());
	strncpy(to_send.oper, s, 2);
	to_send.l_op = l;
	to_send.r_op = r;
	to_send.has_pr_id = 1;
	to_send.pr_id = getpid();

	m_len = calculation__get_packed_size(&to_send);
	buff = malloc(m_len);
	calculation__pack(&to_send, buff);

	flag = mq_send(qd_server, buff, m_len, 0);
	if (flag == -1)
		perror("Unable to queue the message.");
	mq_close(qd_server);

}
void receive_queued(enum client_mode type) {

	Calculation *receive;
	void *buffer;
	buffer = malloc(MAX_QUEUE_MSG_SIZE);
	size_t m_len;
	while (1) {
		m_len = mq_receive(qd_client, buffer, MAX_QUEUE_MSG_SIZE, 0);

		if (m_len == -1) {
			//perror("Message not received.");
			break;
		}
		receive = calculation__unpack(NULL, m_len, buffer);
		if (receive->has_answer) {
			printf("\nServer async response: %lf %s %lf = %lf\n", receive->l_op,
					receive->oper, receive->r_op, receive->answer);
		}
	}
}

void read_log() {
	int fd;
	Calculation *current;
	uint8_t *buffer;
	struct stat sb;
	fd = shm_open(SHMOBJ_PATH, O_CREAT | O_RDWR, S_IRWXU | S_IRWXG);
	if (fd < 0) {
		perror("In shm_open()");
		exit(1);
	}

	if (fstat(fd, &sb) == -1)
		perror("Couldn't obtain size.");

	if (sb.st_size) {

		ftruncate(fd, sb.st_size);
		buffer = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);

		current = calculation__unpack(NULL, sb.st_size, buffer);

		if (current && current->other_message) {
			printf("%s", current->other_message);
		} else if (current && !current->other_message){
			printf("\nReceived something but there was no message to read.\n");
		}

	}
	close(fd);
}

void *listener() {
	do {
		sleep(2);
		receive_queued(NORMAL);
	} while (1);
}
void post_monitoring_state(protobuf_c_boolean state) {
	void *buff;
	Calculation to_send = CALCULATION__INIT;
	size_t m_len;
	int flag;

	qd_server = mq_open(QD_SERVER_NAME, O_CREAT | O_WRONLY | O_NONBLOCK, 0664,
	NULL);
	if (qd_server == -1)
		perror("Couldn't open server's message queue");

	to_send.sem_name = malloc(MAX_SEM_NAME);
	if (client_sem_name[0] != '\0')
		strcpy(to_send.sem_name, client_sem_name);
	to_send.has_monitor = state;
	to_send.monitor = state;
	to_send.has_pr_id = 1;
	to_send.pr_id = getpid();

	m_len = calculation__get_packed_size(&to_send);
	buff = malloc(m_len);
	calculation__pack(&to_send, buff);

	flag = mq_send(qd_server, buff, m_len, 0);
	if (flag == -1)
		perror("Unable to queue the message.");
	mq_close(qd_server);
}
