#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>


#define BUF_LEN			1024
#define POLLING_TIME	1
#define MAX_RETR		10		// numero di ritrasmissioni consentito
#define MAX_NEIGHBORS	2		// ogni peer ha solo due neighbor
#define	REQ_MSG			"REQ\0"	// messaggio di richiesta di connessione
#define	ACK_MSG			"ACK\0"	// messaggio di conferma dimensione lista ricevuta
#define THX_MSG			"THX\0" // messaggio di conferma lista ricevuta
#define LST_MSG			"LST\0" // messaggio di invio lista
#define ESC_MSG			"ESC\0" // messaggio di chiusura DS
#define DNG_MSG			"DNG\0" // messaggio di disconnessione inviato dai peers
#define RPN_MSG			"RPN\0" // messaggio di richiesta numero di peers della rete
#define	MSG_LEN			4		// lunghezza predefinita dei messaggi

typedef struct peer* peer_list;
typedef uint16_t msg_size;

struct peer {
	struct sockaddr_in peer_addr;
	peer_list neighbors[MAX_NEIGHBORS]; //neighbors[0]: previous; neighbors[1]: next
};


/* Converte un intero in stringa */
char *my_itoa(int num, char *str) {
	if(str == NULL) {
		return NULL;
	}
	sprintf(str, "%d", num);
	return str;
}


/* Cerca un peer nella lista e se presente ritorna il puntatore a tale elemento */
struct peer* find_peer(int n_port, peer_list lista, int n_peer){
	int i;
	struct peer* p;
	for (i = 0, p = lista; i < n_peer && p != NULL; i++, p = p->neighbors[1]) {
		if (p->peer_addr.sin_port == n_port) {
			return p;
		}
	}
	return NULL;
}


/* Copia la lista dei neighbors in tmp e ritorna la dimensione in bytes */
int find_neighbors(int n_port, peer_list lista, int n_peer, struct sockaddr_in* tmp){
	int i, ret;
	char buff[BUF_LEN];
	char port_buff[BUF_LEN];
	struct peer* p;

	// pulizia struttura temp_neighbors
	memset(tmp, 0, sizeof(struct sockaddr_in) * MAX_NEIGHBORS);

	if (n_peer == 0) {
		return 0;
	}
	if (n_peer == 1) {
		// se l'unico peer connesso e' n_port, vuol dire che non ha neighbors
		if (n_port == lista->peer_addr.sin_port)
			return 0;
		if (inet_ntop(AF_INET, &lista->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
			fprintf(stderr, "Indirizzo del previous neighbor non valido\n");
			exit(1);
		}
		my_itoa(lista->peer_addr.sin_port, port_buff);
		ret = strlen(buff) + strlen(port_buff) + 2 + strlen("0.0.0.0 0;");
		if (n_port > lista->peer_addr.sin_port) {
			tmp[0] = lista->peer_addr;
		} else {
			tmp[1] = lista->peer_addr;
		}
	}
	else {	
	// se i peer sono almeno due, scorro la lista in cerca dei neighbors di n_port
		for (i = 1, p = lista, ret = 0; i <= n_peer && p != NULL; i++, p = p->neighbors[1]) {

		// se sono gia' fra i peer registrati..
			if (n_port == p->peer_addr.sin_port) {
				// ..se non sono il minore in una lista di due, prendo il neighbor precedente
				if (!(n_peer == 2 && i == 1)) {
					if (inet_ntop(AF_INET, &p->neighbors[0]->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
						fprintf(stderr, "Indirizzo del previous neighbor non valido\n");
						exit(1);
					}
					my_itoa(p->neighbors[0]->peer_addr.sin_port, port_buff);
					ret += (strlen(buff) + strlen(port_buff) + 2);
					tmp[0] = p->neighbors[0]->peer_addr;
				}
				// ..se non sono il maggiore in una lista di due, prendo il neighbor successivo
				if (!(n_peer == 2 && i == n_peer)) {
					if (inet_ntop(AF_INET, &p->neighbors[1]->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
						fprintf(stderr, "Indirizzo del next neighbor non valido\n");
						exit(1);
					}
					my_itoa(p->neighbors[1]->peer_addr.sin_port, port_buff);
					ret += (strlen(buff) + strlen(port_buff) + 2);
					tmp[1] = p->neighbors[1]->peer_addr;
				}
				// se sono in una lista di due, aggiungo a ret la dimensione dell'indirizzo nullo
				if (n_peer == 2) {
					ret += strlen("0.0.0.0 0;");
				}

				break;
			}

		// se non sono fra i peer registrati, scorro la lista finche' o sono il maggiore in assoluto o trovo un peer minore
			if (n_port < p->peer_addr.sin_port || i == n_peer) {
				// stiamo usando una lista ciclica, percio' per semplificare il codice
				// se n_peer e' il maggiore in assoluto consideriamolo come se fosse in cima alla lista
				if (i == n_peer && n_port > p->peer_addr.sin_port) {
					p = lista;
				}
				// se la lista ha piu' di due peer, ci limitiamo a prendere i due peer adiacenti a noi come neighbor
				if (n_peer > 2) {
					if (inet_ntop(AF_INET, &p->neighbors[0]->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
						fprintf(stderr, "Indirizzo del previous neighbor non valido\n");
						exit(1);
					}
					my_itoa(p->neighbors[0]->peer_addr.sin_port, port_buff);
					ret += (strlen(buff) + strlen(port_buff) + 2);
					tmp[0] = p->neighbors[0]->peer_addr;

					if (inet_ntop(AF_INET, &p->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
						fprintf(stderr, "Indirizzo del next neighbor non valido\n");
						exit(1);
					}
					my_itoa(p->peer_addr.sin_port, port_buff);
					ret += (strlen(buff) + strlen(port_buff) + 2);
					tmp[1] = p->peer_addr;
				}
				// se la lista ha esattamente due peer, dobbiamo ricordarci che ogni peer ha solo un neighbor:
				// per decidere il neighbor precedente e quello successivo dobbiamo prima capire la nostra posizione in lista 
				if (n_peer == 2) {
					if (inet_ntop(AF_INET, &lista->neighbors[1]->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
						fprintf(stderr, "Indirizzo del previous neighbor non valido\n");
						exit(1);
					}
					my_itoa(lista->neighbors[1]->peer_addr.sin_port, port_buff);
					ret += (strlen(buff) + strlen(port_buff) + 2);

					if (inet_ntop(AF_INET, &lista->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
						fprintf(stderr, "Indirizzo del next neighbor non valido\n");
						exit(1);
					}
					my_itoa(lista->peer_addr.sin_port, port_buff);
					ret += (strlen(buff) + strlen(port_buff) + 2);

					if (n_port > lista->peer_addr.sin_port && n_port < lista->neighbors[1]->peer_addr.sin_port) {
						tmp[0] = lista->peer_addr;
						tmp[1] = lista->neighbors[1]->peer_addr;
					} 
					else {
						tmp[0] = lista->neighbors[1]->peer_addr;
						tmp[1] = lista->peer_addr;
					}

				}

				break;
			}
		}	// fine for
	}
	return ret;
}


/* Rimuove peer dalla rete */
int del_peer(int n_port, peer_list* p_lista, int* n_peer){
	struct peer* p;

	// verifico se il peer e' in lista e cerco la sua posizione
	p = find_peer(n_port, *p_lista, *n_peer);
	if (p == NULL) {
		return -1;
	}	

	// se sto rimuovendo il peer minore, il puntatore di testa della lista punta al successivo peer (che sara' il piu' piccolo dei rimanenti)
	if (*n_peer > 1 && p->neighbors[0]->peer_addr.sin_port > p->peer_addr.sin_port) {
		*p_lista = p->neighbors[1];
	}

	// aggiusto i neighbors del peer precedente e del peer successivo al peer da rimuovere
	if (*n_peer == 2) {
		p->neighbors[0]->neighbors[0] = NULL;
		p->neighbors[0]->neighbors[1] = NULL;
	}
	else if (*n_peer > 2) {
		p->neighbors[1]->neighbors[0] = p->neighbors[0];
		p->neighbors[0]->neighbors[1] = p->neighbors[1];
	}
	
	// libero la memoria e decremento il numero di peer in lista
	free(p);
	(*n_peer)--;
	printf("PEER %d RIMOSSO\n", n_port);
	return 0;
}


/* Registra nuovo peer */
int reg_peer(struct sockaddr_in new_addr, peer_list* p_lista, int* n_peer){
	int i, ret;
	char buff[BUF_LEN];
	struct peer *p, *new_peer;

	// Creo un nuovo oggetto peer con campo peer_addr = new_addr
	new_peer = (struct peer*)malloc(sizeof(new_addr) + sizeof(struct peer*) * MAX_NEIGHBORS);
	if (new_peer == NULL) {
		fprintf(stderr, "\nImpossibile allocare memoria per un nuovo peer\n");
		exit(1);
	}
	new_peer->peer_addr = new_addr;
	new_peer->peer_addr.sin_port = ntohs(new_addr.sin_port);
	
	// Se il nuovo peer e' il primo del network, il puntatore di testa punta al nuovo peer
	if (*p_lista == NULL) {	
		*p_lista = new_peer;
		for (i = 0; i < MAX_NEIGHBORS; i++)
			new_peer->neighbors[i] = NULL;
		(*n_peer)++;
		return 0;
	}

	p = *p_lista;

	// Se e' il secondo peer del network, mi aggancio semplicemente al primo peer
	if (*n_peer == 1) {
		if (new_peer->peer_addr.sin_port == p->peer_addr.sin_port) {
			fprintf(stderr, "Peer gia' registrato\n");
			return -1;
		}
		new_peer->neighbors[0] = p;
		new_peer->neighbors[1] = p;
		p->neighbors[0] = new_peer;
		p->neighbors[1] = new_peer;
		// se ha n.porta minore, lo faccio puntare dal puntatore di testa
		if (new_peer->peer_addr.sin_port < p->peer_addr.sin_port)
			*p_lista = new_peer;
	} 
	// Se ci sono almeno due peer, scorro la lista per piazzare il nuovo peer nel punto giusto
	else {
		for (i = 1; i <= *n_peer && p != NULL; i++, p = p->neighbors[1]) {
			if (new_peer->peer_addr.sin_port == p->peer_addr.sin_port) {
				fprintf(stderr, "Peer gia' registrato\n");
				return -1;
			}
			// appena trovo un peer con n.porta maggiore, inserisco il nuovo peer prima di esso
			if (new_peer->peer_addr.sin_port < p->peer_addr.sin_port) {
				new_peer->neighbors[0] = p->neighbors[0];
				new_peer->neighbors[1] = p;
				p->neighbors[0]->neighbors[1] = new_peer;
				p->neighbors[0] = new_peer;
				// se il nuovo peer ha n.porta minore in assoluto, lo faccio puntare dal puntatore di testa
				if (i == 1)
					*p_lista = new_peer;
				break;
			} 
			else if (i == *n_peer) { // se il nuovo peer ha n.porta maggiore in assoluto, lo metto in fondo
				new_peer->neighbors[0] = p;
				new_peer->neighbors[1] = p->neighbors[1];
				p->neighbors[1]->neighbors[0] = new_peer;
				p->neighbors[1] = new_peer;
				break;
			}
		}
	}

	(*n_peer)++;
	printf("PEER REGISTRATO\n");
	return 0;
}


/* Invio della lista di neighbors al peer con indirizzo connecting_addr */
int send_list(int sd, struct sockaddr_in connecting_addr, peer_list peer_connessi, int num_peer, struct sockaddr_in* tmp_neighbors) {
	int ret;
	int count;								// Variabile di conteggio
	msg_size lmsg;							// Dimensione dei messaggi
	char buffer[BUF_LEN];					// Buffer
	char addr0[BUF_LEN];
	char addr1[BUF_LEN];
	struct sockaddr_in tmp[MAX_NEIGHBORS];

	// nel caso non voglio modificare alcuna struttura tmp_neighbors, posso passare alla funzione tmp_neighbors = NULL
	if (tmp_neighbors == NULL) {
		tmp_neighbors = tmp;
	}

	// Calcolo i bytes necessari a contenere gli indirizzi dei neighbors
	lmsg = find_neighbors(ntohs(connecting_addr.sin_port), peer_connessi, num_peer, tmp_neighbors);

	// Invio la dimensione degli indirizzi dei neighbors al peer
	lmsg = htons(lmsg);
	ret = send(sd, (void*)&lmsg, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send() fallita: dimensione indirizzi dei neighbors non trasmessa\n");	// DEBUG
		sleep(POLLING_TIME);
	}

	// Aspetto l'ACK da parte del peer (per assicurarmi che abbia ricevuto la dimensione del prossimo messaggio).
	// Dopodiche' invio periodicamente la lista dei neighbors finche' il peer non notifica di aver ricevuto la lista
	count = 0;	// resetto il contatore di ritrasmissioni
	do {
		ret = recv(sd, buffer, MSG_LEN, 0);
		if (ret < 0 ) {
			count++;
			fprintf(stderr, "recv() fallita\n");	// DEBUG
			sleep(POLLING_TIME);
		}
		else { 
			// Se il peer conferma la ricezione della lista, esco dal loop
			if (strcmp(buffer, THX_MSG) == 0){
				printf("Conferma ricezione lista ricevuta\n");
				ret = 1;
				continue;
			}
			// Se ricevo l'ACK, invio la lista dei neighbors al peer
			if (strcmp(buffer, ACK_MSG) == 0){
				printf("ACK ricevuto\n");	// DEBUG
				if (lmsg == 0) {
					ret = 1;
					continue;
				}
				// Recupero la lista
				inet_ntop(AF_INET, &tmp_neighbors[0].sin_addr, addr0, sizeof(addr0));
				inet_ntop(AF_INET, &tmp_neighbors[1].sin_addr, addr1, sizeof(addr1));
				sprintf(buffer, "%s %d;%s %d;", addr0, tmp_neighbors[0].sin_port, addr1, tmp_neighbors[1].sin_port);
				printf("Invio lista (%dbytes): %s\n", strlen(buffer), buffer);	// DEBUG
				// Invio la lista
				ret = send(sd, (void*)buffer, lmsg, 0);
				if (ret < 0){
					count++;
					fprintf(stderr, "send() fallita: lista non trasmessa\n");	// DEBUG
					sleep(POLLING_TIME);
				}
			}
			// Se ricevo un messaggio errato
			else {
				printf("ricevuto messaggio errato %s\n", buffer);	// DEBUG
				count++;
				sleep(POLLING_TIME);
			}
			ret = -1;
		}

	} while (ret < 0 && count < MAX_RETR);

	printf("%d ritrasmissioni\n", count); //DEBUG
	return ret;
}


/* Stampa a video una guida dei comandi */
void help(){
	printf("1) help                --> mostra i dettagli dei comandi\n");
	printf("2) showpeers           --> mostra un elenco dei peer connessi\n");
	printf("3) showneighbor <peer> --> mostra i neighbor di un peer\n");
	printf("4) esc                 --> chiude il DS\n\n");
}


/* Stampa a video la schermata di avvio */
void start_screen(int ds_port){
	int i;
	printf("\n");
	for (i = 0; i < 61; i++) {
		if (i == 30)
			printf(" DS COVID STARTED ");
		else
			printf("*");
	}
	printf("\nPorta DS: %d\nDigita un comando:\n", ds_port);
	help();
}


/* Stampa a video l'elenco dei peer connessi */
void showpeers(struct peer* p, int n_peer){
	int i;
	char buff[BUF_LEN];
	if (n_peer == 0) {
		printf("Nessun peer connesso\n");
	} else {
		printf("Peer connessi: %d\n", n_peer);
		for (i = 0; i < n_peer && p != NULL; i++, p = p->neighbors[1]) {
			if (inet_ntop(AF_INET, &p->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
				fprintf(stderr, "Indirizzo del next neighbor non valido\n");
				exit(1);
			}
			printf(" Indirizzo %s ", buff);
			printf(" Porta %d\n", p->peer_addr.sin_port);
		}
	}
	printf("\n");
}


/* Stampa a video l'elenco dei neighbors */
void showneighbor(peer_list lista, int n_peer, char* peer_addr, int n_port) {
	int i, j, ret;
	struct peer* p = lista;
	struct sockaddr_in tmp[MAX_NEIGHBORS];
	char buff[BUF_LEN];

	if (n_port != 0) {
		// prima controllo che il peer sia effettivamente connesso
		p = find_peer(n_port, lista, n_peer);
		if (p == NULL) {
			printf("Peer %s:%d non connesso\n\n", peer_addr, n_port);
			return;
		}

		find_neighbors(n_port, lista, n_peer, tmp);
		printf("Peer %s:%d", peer_addr, n_port);
		for (i = 0; i < MAX_NEIGHBORS; i++) {
			memset(buff, 0, sizeof(buff));
			if (inet_ntop(AF_INET, &tmp[i].sin_addr, buff, sizeof(buff)) == NULL) {
				fprintf(stderr, "Indirizzo del next neighbor non valido\n");
				exit(1);
			}
			printf("\t[%d] %s:%d;", i, buff, tmp[i].sin_port);
		}
		printf("\n");
	}
	// se il peer non e' stato specificato (n_port == 0), allora stampa le liste di tutti i peer
	else {
		for (j = 0; j < n_peer && p != NULL; j++, p = p->neighbors[1]) {
			find_neighbors(p->peer_addr.sin_port, lista, n_peer, tmp);

			if (inet_ntop(AF_INET, &p->peer_addr.sin_addr, buff, sizeof(buff)) == NULL) {
				fprintf(stderr, "Indirizzo del next neighbor non valido\n");
				exit(1);
			}
			printf("Peer %s:%d", buff, p->peer_addr.sin_port);

			for (i = 0; i < MAX_NEIGHBORS; i++) {
				memset(buff, 0, sizeof(buff));
				if (inet_ntop(AF_INET, &tmp[i].sin_addr, buff, sizeof(buff)) == NULL) {
					fprintf(stderr, "Indirizzo del next neighbor non valido\n");
					exit(1);
				}
				printf("\t[%d] %s:%d;", i, buff, tmp[i].sin_port);
			}
			printf("\n");
		}
	}
	printf("\n");
}

/* Termina il DS e tutti i peer */
int esc(int sd, peer_list* p_lista, int* n_peer){
	int i, n, ret;
	struct peer* p;
	struct peer temp;
	n = *n_peer;
	for (i = 0, p = *p_lista; i < n && p != NULL; i++, p = p->neighbors[1]) {
		temp.peer_addr = p->peer_addr;
		temp.peer_addr.sin_port = htons(p->peer_addr.sin_port);
		do {
			ret = sendto(sd, ESC_MSG, MSG_LEN, 0, (struct sockaddr*)&temp.peer_addr, sizeof(temp.peer_addr));
			if (ret < 0) {
				fprintf(stderr, "sendto(): ESC_MSG non trasmesso al peer %d", temp.peer_addr.sin_port);
				sleep(POLLING_TIME);
			}
		} while (ret < 0);
		del_peer(p->peer_addr.sin_port, p_lista, n_peer);
	}
}


/******************************************************** MAIN ********************************************************/

int main(int argc, char* argv[]){
	int i;
	int ret;										// Variabile per il controllo dei valori di ritorno delle funzioni
	int addrlen;									// Dimensione di buffer e indirizzi
	msg_size tmps;									// Variabile temporanea dove salvare uint16_t

	int sd, fd_stdin;								// Descrittori di socket e stdin
	fd_set master;									// Set di descrittori da monitorare in lettura
	fd_set read_fds;								// Set di descrittori pronti in lettura
	int fdmax;										// Descrittore max

	int port;										// Variabile per salvare i numeri di porta
	int num_peer = 0;								// Numero di peer connessi
	char buffer[BUF_LEN];							// Buffer
	struct sockaddr_in my_addr, connecting_addr;	// Strutture per gli indirizzi del server e del client
	struct sockaddr_in unconnecting_addr;			// Indirizzo usato per annullare la connect()
	struct sockaddr_in tmp_neighbors[MAX_NEIGHBORS];// Struttura per salvare temporaneamente gli indirizzi dei neighbors
	peer_list peer_connessi = NULL;					// Lista dei peer connessi


	/* Acquisizione della porta associata al discovery server */
	 // Il DS si avvia con il comando	./ds <porta>
	 // dunque argv[1] punta alla stringa contenente la porta associata al DS
	if (argv[1] == NULL) {
		fprintf(stderr, "Nessuna porta associata al discovery server\n");
		exit(1);
	}
	ret = atoi(argv[1]);	// atoi() converte una stringa in un intero
	if (ret <= 0) {
		fprintf(stderr, "Porta DS non valida\n");
		exit(1);
	}
	port = ret;

	// Creazione indirizzo per annullare la connect()
	memset(&unconnecting_addr, 0, sizeof(unconnecting_addr));
	unconnecting_addr.sin_family = AF_UNSPEC;

	/* Creazione socket UDP non bloccante */
	sd = socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK, 0);

	/* Creazione indirizzo di bind */
	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(port);
	my_addr.sin_addr.s_addr = INADDR_ANY;

	/* Aggancio del socket all'indirizzo di bind */
	ret = bind(sd, (struct sockaddr*)&my_addr, sizeof(my_addr));
	if (ret < 0) {
		fprintf(stderr, "\nBind non riuscita: %s\n", strerror(errno));
		exit(1);
	}

	/* Stampa la schermata di avvio */
	start_screen(port);

	/* Lunghezza dell'indirizzo del client */
	addrlen = sizeof(connecting_addr);

	/* File Descriptor dello standard input*/
	fd_stdin = fileno(stdin);

	// Reset dei descrittori
	FD_ZERO(&master);
	FD_ZERO(&read_fds);

	// Aggiungo stdin e listener socket ai descrittori monitorati
	FD_SET(fd_stdin, &master);
	FD_SET(sd, &master);

	// Tengo traccia del nuovo fdmax
	if (fd_stdin > sd) {
		fdmax = fd_stdin;
	} else {
		fdmax = sd;
	}

	while(1) {

		read_fds = master;

		memset(buffer, 0, BUF_LEN); // Pulizia del buffer

		/* Utilizzo la select() per rendere non-bloccante la lettura dallo stdin */
		ret = select(fdmax + 1, &read_fds, NULL, NULL, NULL);
		if (ret == -1) {
			fprintf(stderr, "\nErrore nella select: %s\n", strerror(errno));
			exit(1);
		} 
	
		/* Se qualcosa e' stato scritto nello standard input, controlliamo che sia uno dei comandi accettati dal DS	*/
		if (FD_ISSET(fd_stdin, &read_fds)) {

			ret = read(fd_stdin, buffer, BUF_LEN);
			if (ret < 0) {
				fprintf(stderr, "\nErrore nella read: %s\n", strerror(errno));
				exit(1);
			}
			if (strcmp(buffer, "help\n\0") == 0){
				help();
			}
			if (strcmp(buffer, "showpeers\n\0") == 0){
				showpeers(peer_connessi, num_peer);
			}
			if (strcmp(buffer, "esc\n\0") == 0){
				esc(sd, &peer_connessi, &num_peer);
				break;
			}

			if (strcmp(buffer, "showneighbor\n\0") == 0){
				showneighbor(peer_connessi, num_peer, "127.0.0.1", 0);	// se non specifico il peer, mostro tutte le liste
			}
			ret = sscanf(buffer, "showneighbor %d", &port);
			if (ret > 0) {				
				showneighbor(peer_connessi, num_peer, "127.0.0.1", port);
			}

		}

		/* Gestiamo le di eventuali richieste di connessione */
		if (FD_ISSET(sd, &read_fds)) {
			ret = recvfrom(sd, buffer, MSG_LEN, 0, (struct sockaddr*)&connecting_addr, &addrlen);
			if (ret < 0) {
				fprintf(stderr, "\nErrore nella recvfrom: %s\n", strerror(errno));
				exit(1);
			}

			// Se il messaggio arrivato sul socket e' REQ_MSG, devo gestire la registrazione di un nuovo peer alla rete
			if (strcmp(buffer, REQ_MSG) == 0) {

				// Socket UDP "connesso": comunico con un solo peer alla volta e non devo specificarne ogni volta l'indirizzo
				ret = connect(sd, (struct sockaddr*)&connecting_addr, sizeof(connecting_addr));
				if (ret < 0) {
					fprintf(stderr, "Errore: connect() fallita\n");
					exit(1);
				}

				/**/////////////////////////////////////// DEBUG ////////////////////////////////////////////**/
				/**/memset(buffer, 0, BUF_LEN);																/**/
				/**/inet_ntop(AF_INET, &connecting_addr.sin_addr, buffer, sizeof(buffer));					/**/
				/**/printf("\nRichiesta valida dal peer %s:%d\n", buffer, ntohs(connecting_addr.sin_port));	/**/
				/**//////////////////////////////////////////////////////////////////////////////////////////**/

				// Mando la lista al nuovo peer
				ret = send_list(sd, connecting_addr, peer_connessi, num_peer, tmp_neighbors);

				// ret vale 1 solo se il nuovo peer ha accettato la lista inviandomi il THX_MSG
				if (ret == 1) { 

					// Registro il peer
					reg_peer(connecting_addr, &peer_connessi, &num_peer);

					// Invio ai neighbors la lista modificata dall'arrivo del nuovo peer
					for (i = 0; i < MAX_NEIGHBORS; i++) {
						if (tmp_neighbors[i].sin_addr.s_addr != 0){
							printf("\nAggiorna lista neighbor [%d] %d\n", i, tmp_neighbors[i].sin_port); /////////////// DEBUG
							tmp_neighbors[i].sin_port =  htons(tmp_neighbors[i].sin_port);
							connecting_addr = tmp_neighbors[i];
							// Socket UDP "connesso": comunico solo con il neighbor i-esimo
							ret = connect(sd, (struct sockaddr*)&connecting_addr, sizeof(connecting_addr));
							if (ret < 0) {
								fprintf(stderr, "Errore: connect() fallita\n");
								exit(1);
							}
							// Avviso il neighbor i-esimo che sto per inviargli una nuova lista
							do {
								ret = send(sd, LST_MSG, MSG_LEN, 0);
								if (ret < 0) {
									fprintf(stderr, "sendto() fallita: aggiornamento lista non trasmessa\n");	// DEBUG
									sleep(POLLING_TIME);
								}
								else {
									// Mando la lista al neighbor i-esimo
									ret = send_list(sd, connecting_addr, peer_connessi, num_peer, NULL);
								}
							} while (ret < 0);

						}
					}

				}

				// "Disconnetto" il socket UDP: uscito dal loop, devo poter ricevere richieste da un qualsiasi indirizzo
				if (connect(sd, (struct sockaddr *)&unconnecting_addr, sizeof(unconnecting_addr)) < 0) {
					fprintf(stderr, "Errore: connect() fallita\n");
					exit(1);
				}

			}


			else if (strcmp(buffer, DNG_MSG) == 0) {
				printf("Richiesta di disconnessione dal peer %d\n", ntohs(connecting_addr.sin_port)); // DEBUG
				// disconnetti il peer
				ret = del_peer(ntohs(connecting_addr.sin_port), &peer_connessi, &num_peer);
				if (ret < 0) {
					printf("Peer non presente\n");
					continue;
				}
			}


			else if (strcmp(buffer, RPN_MSG) == 0) {
				printf("Richiesto numero di peer connessi dal peer %d\n", ntohs(connecting_addr.sin_port)); // DEBUG

				// Socket UDP "connesso": comunico con un solo peer alla volta e non devo specificarne ogni volta l'indirizzo
				ret = connect(sd, (struct sockaddr*)&connecting_addr, sizeof(connecting_addr));
				if (ret < 0) {
					fprintf(stderr, "Errore: connect() fallita\n");
					exit(1);
				}

				tmps = htons(num_peer);
				do {
					ret = send(sd, (void*)&tmps, sizeof(msg_size), 0);
					if (ret < 0) {
						fprintf(stderr, "sendto() fallita: numero peers connessi non trasmesso\n");	// DEBUG
						sleep(POLLING_TIME);
					}
				} while (ret < 0);
				printf("Count %d\n", num_peer);

				// "Disconnetto" il socket UDP: uscito dal loop, devo poter ricevere richieste da un qualsiasi indirizzo
				if (connect(sd, (struct sockaddr *)&unconnecting_addr, sizeof(unconnecting_addr)) < 0) {
					fprintf(stderr, "Errore: connect() fallita\n");
					exit(1);
				}
			}


			// Non considerare richieste provenienti dai peer se il primo messaggio e' diverso da "REQ\0"
			else {
				/**/////////////////////////////////////// DEBUG ////////////////////////////////////////////////**/
				/**/memset(buffer, 0, BUF_LEN);																	/**/
				/**/inet_ntop(AF_INET, &connecting_addr.sin_addr, buffer, sizeof(buffer));						/**/
				/**/printf("\nRichiesta non valida dal peer %s:%d\n", buffer, ntohs(connecting_addr.sin_port));	/**/
				/**//////////////////////////////////////////////////////////////////////////////////////////////**/
			}

			
		} 

	}
	
	/* Chiusura del server */	
	close(sd);
}
