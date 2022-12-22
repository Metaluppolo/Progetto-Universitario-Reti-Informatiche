#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <regex.h>
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
#define RNG_MSG			"RNG\0" // messaggio di invio register ad un neighbor
#define REQ_DATA		"RQD\0"	// messaggio di richiesta di file con dati aggregati calcolati
#define FLOOD_FOR_ENTRIES "FFE\0" // messaggio di richiesta di flooding
#define REPLY_FLOOD		"RPF\0"	// messaggio di risposta al flooding
#define REPLY_END		"RPE\0"	// messaggio di fine risposta al flooding
#define FLOOD_END		"FLE\0"	// messaggio di fine flooding
#define RPN_MSG			"RPN\0" // messaggio di richiesta numero di peers della rete
#define	MSG_LEN			4		// lunghezza predefinita dei messaggi
#define MAX_CMD_ARG		4		// numero massimo di argomenti di un comando passato da standard input

typedef uint16_t msg_size;

struct addr_temp{
	int port;
	char ip[BUF_LEN];
};

struct subperiod {
	uint32_t begin;
	uint16_t days;
};


/* Converte un intero in stringa */
char *my_itoa(int num, char *str) {
	if(str == NULL) {
		return NULL;
	}
	sprintf(str, "%d", num);
	return str;
}


int make_dir(int peer, char *subdir) {
	char dir[BUF_LEN];		// Buffer per il nome della directory
	struct stat st = {0};	// Struttura per verificare che la directory esista
	// Se non esiste gia', creo la directory relativa al peer
	sprintf(dir, "./%d", peer);
	if (stat(dir, &st) == -1) {
		mkdir(dir, 0700);
		if (subdir == NULL) {
			return 0;
		}
	}
	// Se non esiste, creo la sotto-directory ./peer/subdir
	if (subdir != NULL) {
		memset(dir, 0, BUF_LEN);	// pulisco buffer
		sprintf(dir, "./%d/%s", peer, subdir);
		if (stat(dir, &st) == -1) {
			mkdir(dir, 0700);
			return 0;
		}
	}
	return 1;
}


/* Stampa a video la schermata di avvio */
void start_screen(int ds_port){
	int i;
	printf("\n");
	for (i = 0; i < 61; i++) {
		if (i == 30)
			printf(" PEER COVID STARTED ");
		else
			printf("*");
	}
	printf("\nPorta: %d\nDigita un comando:\n", ds_port);
	printf("1) start <DS_addr> <DS_port>  --> richiede al DS la connessione al network\n");
	printf("2) add <type> <quantity>      --> aggiunge una entry al register con data odierna del peer\n");
	printf("3) get <aggr> <type> <period> --> richede l'elaborazione di un dato aggregato\n");
	printf("4) stop                       --> disconnette il peer\n\n");
}


/* Invia un register ad un peer connesso */
int send_register(int port, int sd, int port_rgst, struct tm *ptmi, char *buff) {
	int ret;
	msg_size lmsg;
	FILE *fptr;
	char file[BUF_LEN];

	// Invio messaggio di richiesta RNG_MSG
	ret = send(sd, RNG_MSG, MSG_LEN, 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio RNG_MSG\n");
		exit(1);
	}
	printf("Inviato RNG_MSG tramite socket %d\n", sd);

	// Invio porta del peer proprietario del register
	lmsg = htons(port_rgst);
	ret = send(sd, (void*)&lmsg, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio nome register\n");
		exit(1);
	}

	// Invio nome file in formato standardizzato %4d:%02d:%02d (10 caratteri)
	sprintf(file, "%4d:%02d:%02d", ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
	ret = send(sd, (void*)file, strlen(file), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio nome register\n");
		exit(1);
	}
	printf("Inviato nome del register(%dbytes): %s\n", strlen(file), file);

	// Ricevo disponibilita' del register dal peer destinatario
	ret = recv(sd, (void*)&lmsg, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "recv(): errore ricezione disponibilita' dal peer destinatario\n");
		exit(1);
	}
	lmsg = ntohs(lmsg);
	if (lmsg != 1) {
		printf("Il peer destinatario possiede gia' il register\n");
		return;
	}

	// Invio dimensione del register
	lmsg = htons( strlen(buff) );
	ret = send(sd, (void*)&lmsg, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio dimensione register\n");
		exit(1);
	}
	// Invio contenuto del register
	ret = send(sd, (void*)buff, strlen(buff), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio register\n");
		exit(1);
	}
	printf("Inviato contenuto del register(%dbytes)\n", strlen(buff));
}


/* Ricevi un register da un peer connesso */
int recv_register(int port, int sd, int sin_port) {
	int ret, len;
	int port_rgst;
	msg_size lmsg;
	char buff[BUF_LEN];
	char file[BUF_LEN];
	char *line = NULL;
	size_t linelen;
	ssize_t read;
	FILE *fptr;
	
	// Se non esiste gia', creo la directory relativa al peer
	make_dir(port, "registers");

	// ricevi nome register
	ret = recv(sd, (void*)&lmsg, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "recv(): errore ricezione nome register\n");
		exit(1);
	}
	port_rgst = ntohs(lmsg);

	memset(buff, 0, BUF_LEN); // pulizia buffer
	ret = recv(sd, (void*)buff, 10, 0);
	if (ret < 0) {
		fprintf(stderr, "recv(): errore ricezione nome register\n");
		exit(1);
	}
	sprintf(file, "./%d/registers/%d %s", port, port_rgst, buff);

	// controllo di non avere gia' il register, altrimenti declino la ricezione del register
	fptr = fopen(file, "r");
	if (fptr != NULL) {
		fclose(fptr);
		printf("Register %s gia' disponibile\n", buff);
		lmsg = 0;
		lmsg = htons(lmsg);
		ret = send(sd, (void*)&lmsg, sizeof(msg_size), 0);
		if (ret < 0) {
			fprintf(stderr, "send(): errore invio disponibilita' register\n");
			exit(1);
		}
		return;
	} 

	lmsg = 1;
	lmsg = htons(lmsg);
	ret = send(sd, (void*)&lmsg, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio disponibilita' register\n");
		exit(1);
	}
	
	printf("Ricezione del register %s dal peer %d...\n", buff, sin_port);

	// ricevi dimensione register
	recv(sd, (void*)&lmsg, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "recv(): errore ricezione dimensione register\n");
		exit(1);
	}
	len = ntohs(lmsg);
	printf("Dimensione register ricevuta: %d\n", len);

	// ricevi register
	recv(sd, (void*)buff, len, 0);
	if (ret < 0) {
		fprintf(stderr, "recv(): errore ricezione contenuto register\n");
		exit(1);
	}
	printf("Register ricevuto\n");

	// crea la copia del register
	fptr = fopen(file, "w");
	if (fptr == NULL) {
		fprintf(stderr, "fopen(): errore");
		exit(1);
	}
	fprintf(fptr, buff);
	fclose(fptr);
	printf("Register copiato\n\n");

	// se non l'ho gia' fatto in passato, inserisci il peer nella lista dei peers da cui ho ricevuto un register:
		// quando andremo a recuperare i files dei registri, cercheremo quelli con questi numeri di porta
		// invece di perdere molto tempo ciclando fra tutti i numeri di porta possibili
	ret = 1;
	memset(file, 0, BUF_LEN);
	sprintf(file, "./%d/PeersList", port);
	fptr = fopen(file, "r");

	// se ancora non esiste il file, scrivo il mio numero di porta
	if (fptr == NULL) {
		fptr = fopen(file, "w");
		if (fptr == NULL) {
			fprintf(stderr, "fopen(): errore");
			exit(1);
		}
		fprintf(fptr, "%d\n", port);
	}
	// altrimenti cerco riga per riga il numero di porta del peer da cui ho ricevuto il register
	else {
		while ((read = getline(&line, &linelen, fptr)) != -1) {
			memset(buff, 0, BUF_LEN);
			line[strcspn(line, "\n")] = 0;
			if (!strcmp(line, my_itoa(port_rgst, buff))) {
				ret = 0;
				break;
			}
		}
	}
	fclose(fptr);

	// se non ho trovato il numero di porta nel file, lo aggiungo in fondo
	if (ret == 1) {
		fptr = fopen(file, "a");
		if (fptr == NULL) {
			fprintf(stderr, "fopen(): errore");
			exit(1);
		}
		fprintf(fptr, "%d\n", port_rgst);
		fclose(fptr);
	}

}


/* Ricevi lista di neighbor dal DS */
int recv_list(int sd, struct sockaddr_in* p_neighbors) {
	int i, ret; 
	int count = 0;						// contatore ritrasmissioni messaggi
	msg_size lmsg;						// dimensione lista
	char buff[BUF_LEN];
	struct addr_temp t[MAX_NEIGHBORS];

	ret = recv(sd, (void*)&lmsg, sizeof(msg_size), 0);	// ricevo dimensione lista
	if (ret == 0) {
		fprintf(stderr, "recv(): socket remoto chiuso\n");
	}
	else if (ret < 0) {
		fprintf(stderr, "recv(): errore ricezione dimensione lista dei neighbors\n");
	}
	else {
	// Se recv() del numero di neighbors va a buon fine...
		lmsg = ntohs(lmsg);

		// Se e' il primo peer a connettersi al DS, non ho bisogno di ricevere alcuna lista di neighbors
		if (lmsg == 0) {
			printf("Nessun neighbor trovato\n");
			for (i = 0; i < MAX_NEIGHBORS; i++)
				memset(&p_neighbors[i], 0, sizeof(p_neighbors[i]));	// azzero i neighbors del peer
		}
		else {
			printf("Ricevuta dimensione lista dei neighbors: %dbytes\n", lmsg);
		}

		// Se altri peer sono gia' connessi al DS tento ciclicamente di spedire un ACK al DS 
		// finche' non ricevo la lista di neighbors "ip1 port1;ip2 port2;...;ipN portN;"
		printf("Trasmissione dell'ACK al DS...\n");
		do {
			ret = send(sd, ACK_MSG, MSG_LEN, 0);
			sleep(POLLING_TIME);
			if (ret < 0) {
				count++;
				fprintf(stderr, "send(): trasmissione ACK fallita\n");
			}
			else if (lmsg > 0) { // Se e' il primo peer a connettersi al DS, non ho bisogno di ricevere alcuna lista di neighbors
				memset(buff, 0, BUF_LEN); // pulizia buff
				printf("ACK trasmesso\nAttesa della lista dei neighbors...\n");
				ret = recv(sd, (void*)buff, lmsg, 0);
				if (ret < 0) {
					count++;
					fprintf(stderr, "recv(): ricezione lista dei neighbors fallita\n");
					sleep(POLLING_TIME);
					continue;
				}
				strcat(buff, ""); // aggiungo il carattere di fine stringa
				printf("Ricevuta lista neighbors (%dbytes): %s\nParsing lista: ", strlen(buff), buff);

				/* parsing del buffer */
				for (i = 0; i < 2; i++) {	// recupero indirizzo e porta di un neighbor alla volta
					ret = sscanf(buff + i * (strchr(buff, ';') - buff + 1), "%s %d;", t[i].ip, &t[i].port);
					if (ret < 2)
						break;	// Se non leggo correttamente tutta la lista, esco dal for
					printf("[%d] %s:%d\t", i, t[i].ip, t[i].port);
					if (i == 1)
						printf("\n");
				}

				// Se leggo una lista errata, continuo a richiederla al DS
				if (ret < 2) {
					printf("Lista dei neighbors errata\n");
					ret = -1;
					sleep(POLLING_TIME);
					continue;
				}
				// Se ricevo correttamente la lista, trasmetto "THX\0" al DS come messaggio di conferma e registro definitivamente il peer
				printf("Invio conferma al DS...\n");
				do {
					ret = send(sd, THX_MSG, MSG_LEN, 0);
					sleep(POLLING_TIME);
					if (ret < 0) {
						count++;
						fprintf(stderr, "send(): trasmissione THX fallita\n");
					}
				} while (ret < 0 && count < MAX_RETR);
			}
		} while (ret < 0 && count < MAX_RETR);

		// Se ho raggiunto il limite massimo di ritrasmissioni, attendere prima di poter richiedere nuovamente una connessione al DS
		if (count >= MAX_RETR) {
			printf("Limite massimo di ritrasmissioni raggiunto\n");
			ret = -1;
			sleep((1 + count) * POLLING_TIME);
		}
		// ..altrimenti la lista che ho ricevuto e' correttamente utilizzabile dal peer
		else if (lmsg > 0){
			for (i = 0; i < MAX_NEIGHBORS; i++) {
				p_neighbors[i].sin_family = AF_INET;
				p_neighbors[i].sin_port = htons(t[i].port);
				ret = inet_pton(AF_INET, t[i].ip, &p_neighbors[i].sin_addr);
				if (ret < 1) {
					fprintf(stderr, "Indirizzo neighbor [%d] non valido\n", i);
					ret = -1;
					sleep((1 + count) * POLLING_TIME);
					continue;
				}
			}
		}
	}
	return ret;
}


/* Richiede al DS la connessione al network */
int start(int sd, struct sockaddr_in* p_srv_addr, char* DS_addr, int DS_port, struct sockaddr_in* p_neighbors){
	int i, ret; 
	int attempt_num = 1;		// contatore tentativi di connessione
	char buff[BUF_LEN];

	if (p_srv_addr->sin_family != 0) {
		fprintf(stderr, "Peer gia' connesso\n");
		return 0;
	}

	/* Creazione indirizzo server */
	memset(p_srv_addr, 0, sizeof(*p_srv_addr));
	p_srv_addr->sin_family = AF_INET;
	p_srv_addr->sin_port = htons(DS_port);
	ret = inet_pton(AF_INET, DS_addr, &p_srv_addr->sin_addr);
	if (ret < 1) {
		fprintf(stderr, "Indirizzo DS non valido\n");
		memset(p_srv_addr, 0, sizeof(*p_srv_addr));
		return 0;
	}

	/* Socked UDP "connesso": comunichiamo solo con il DS e non serve specificarne ogni volta l'indirizzo */
	ret = connect(sd, (struct sockaddr*)p_srv_addr, sizeof(*p_srv_addr));
	if (ret < 0) {
		fprintf(stderr, "Errore: connect() fallita\n");
		return 0;
	}

	/* Invio periodicamente una richiesta di connessione al DS finche' non ricevo una risposta */
	printf("Richiesta di connessione al DS %s:%d in corso...\n", DS_addr, DS_port);
	do {
		printf("\nTENTATIVO %d: \n", attempt_num++);

		// mi assicuro di non avere niente nell' input buffer del socket UDP
		do {
			ret = recv(sd, buff, BUF_LEN, 0);
		} while (ret >= 0);

		printf("Invio messaggio di richiesta...\n");
		ret = send(sd, REQ_MSG, MSG_LEN, 0);
		sleep(POLLING_TIME);
		if (ret < 0) {
			fprintf(stderr, "send(): errore durante l'invio del messaggio di richiesta\n");
		}
		// Se la send() del REQ_MSG va a buon fine, ricevi la lista
		else {
			printf("Messaggio di richiesta inviato\nAttesa dimensione lista dei neighbors...\n");
			ret = recv_list(sd, p_neighbors);
		}
	} while (ret < 0);

	// esco dal ciclo solo quando ho ricevuto una risposta dal DS
	printf("Connesso\n");
	return 1;
}


/* Aggiunge una entry al registro giornaliero */
int add(int peer, char* type, int quantity){
	time_t rawtime;			// Secondi da Epoch
	struct tm *ptm;			// Puntatore a struttura tm
	char entry[BUF_LEN];	// Buffer per la nuova entry
	char file[BUF_LEN];		// Buffer per il nome del register
	FILE *fptr;				// Puntatore a file

	// Recupero l'ora locale
	time(&rawtime);
	ptm = localtime(&rawtime);

	// Formatto la nuova entry
	sprintf(entry, "%4d:%02d:%02d,%s,%d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, type, quantity);
	printf("Nuova entry:\t%s\n", entry);

	// Se non esiste gia', creo la directory relativa al peer
	make_dir(peer, "registers");

	// Se la entry e' aggiunta dopo le 18, va aggiunta al register del giorno successivo
	if (ptm->tm_hour >= 18 && ptm->tm_hour <= 23) {
		rawtime += 86400; // aggiungi numero di secondi in un giorno
		ptm = localtime(&rawtime);
	}

	// Scrivo in fondo al register la nuova entry
	sprintf(file, "./%d/registers/%d %4d:%02d:%02d", peer, peer, ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
	fptr = fopen(file, "a");
	if(fptr == NULL) {
		fprintf(stderr, "fopen(): errore");
		exit(1);
	}
	fprintf(fptr, "%s;", entry);
	fclose(fptr);

	printf("Aggiunta nel register %s\n\n", file);
	return 1;
}


/* Verifica se si puo' calcolare l'aggregato in un period, altrimenti crea lista dei giorni mancanti */
int check_period(int port, char* aggr, char* type, struct tm* ptm_arr, struct subperiod **subper_list) {
	int ret;						// Variabile di ritorno: -1 errore, 0 ho i dati, >0 non ho i dati
	uint16_t count;					// Conta i giorni del sottoperiodo di cui non ho i dati
	uint32_t tmp_begin;				// Variabile temporanea in cui salvo l'inizio del sottoperiodo
	struct tm* ptm;
	struct tm* ptmi;				// Puntatore di scorrimento di strutture tm
	time_t i_t;						// Variabile di scorrimento delle date
	struct tm* ptmnow;				// Puntatore struttura tm di data attuale
	time_t now_t, begin_t, end_t;	// Data attuale, data di inizio period, data di fine period
	char file[BUF_LEN];				// Nome file da aprire
	FILE *fptr;						// Puntatore a file

	ret = 1;

	// Calcolo le strutture time_t relative alle date passate come argomento
	begin_t = mktime(&ptm_arr[0]);
	end_t = mktime(&ptm_arr[1]);

	if (begin_t == -1 || end_t == -1) {
		fprintf(stderr, "mktime(): errore\n");
		ret = -1;
		return ret;
	}
	// Cerco eventuali errori nel period passato come argomento
	time(&now_t);
	ptmnow = localtime(&now_t);
	if (begin_t > end_t || end_t > now_t 
		|| (ptmnow->tm_hour < 18 && ptmnow->tm_hour >= 0 && ptmnow->tm_year == ptm_arr[1].tm_year 
			&& ptmnow->tm_mon == ptm_arr[1].tm_mon && ptmnow->tm_mday == ptm_arr[1].tm_mday)) {
		fprintf(stderr, "Period non valido\n");
		ret = -1;
		return ret;
	}

	// Controllo se ho gia' cio' che mi serve:
	// se non esiste gia', creo la sotto-directory in cui il peer salva i dati aggregati
	make_dir(port, "aggr");
	// Cerco il dato aggregato che mi interessa nella sotto-directory
	sprintf(file, "./%d/aggr/%s %s %4d:%02d:%02d-%4d:%02d:%02d", port, aggr, type, 
			ptm_arr[0].tm_year + 1900, ptm_arr[0].tm_mon + 1, ptm_arr[0].tm_mday, 
			ptm_arr[1].tm_year + 1900, ptm_arr[1].tm_mon + 1, ptm_arr[1].tm_mday);
	fptr = fopen(file, "r");

	// se ho gia' calcolato il dato aggregato, lo restituisco
	if (fptr != NULL) {
		ret = 0;
		fclose(fptr);
		return ret;
	}

	// Se non l'ho gia' calcolato, verifico di avere tutte le entry necessarie per calcolarlo
	/*	NB: Il peer non ricorda quanti e quali peer erano connessi al network al momento t,
		percio' l'unico modo di sapere se abbiamo tutte le entry relative ad un certo giorno e'
		guardare se abbiamo salvato le quantita' totali giornaliere di tamponi o di nuovi casi
		(siamo sicuri che se abbiamo salvato questa info, all'epoca abbiamo contattato tutti i peers connessi in quel momento)
	*/
	// se non esiste gia', creo la sotto-directory in cui il peer salva le quantity
	make_dir(port, "quantities");
	// conta i sottoperiodi di cui non ho le quantity
	for (i_t = begin_t, ret = 0, count = 0; i_t <= end_t; i_t += 86400) {
		ptmi = localtime(&i_t);
		sprintf(file, "./%d/quantities/%s %4d:%02d:%02d", port, type, 
				ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
		fptr = fopen(file, "r");
		if (fptr == NULL) {
			if (count == 0) {
				ret++;
			}
			count++;
		} else {
			fclose(fptr);
			count = 0;
		}
	}

	// se ho trovato almeno un sottoperiodo, crea la lista di sottoperiodi
	if (ret > 0) {
		*subper_list = (struct subperiod*)malloc(ret * sizeof(struct subperiod));
		for (i_t = begin_t, ret = 0, count = 0; i_t <= end_t; i_t += 86400) {
			ptmi = localtime(&i_t);
			sprintf(file, "./%d/quantities/%s %4d:%02d:%02d", port, type, 
					ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
			fptr = fopen(file, "r");
			// se manca una quantita' di <type> in un giorno i_t del period, conteggiala
			if (fptr == NULL) {
				if (count == 0) {
					ret++;
					tmp_begin = i_t + 86400;
				}
				count++;
			} else {
				fclose(fptr);
				if (count > 0) {
					(*subper_list + ret-1)->begin = tmp_begin;
					(*subper_list + ret-1)->days = count - 1;
					count = 0;
				}
			}
		}
		if (count > 0) {
			(*subper_list + ret-1)->begin = tmp_begin;
			(*subper_list + ret-1)->days = count - 1;
		}
	}

	return ret;
}


/* Calcola gli aggregati utilizzando i dati a propria disposizione */
int calc_aggr(int port, char* aggr, char* type, struct tm* ptm_arr, int calc_quantities) {
	int i, ret, count;			// Variabile di scorrimento, di ritorno e di conteggio
	int num_peers;				// Quanti peers mi hanno mandato un register (riduco i cicli per recuperare i registers)
	int *peer;					// Puntatore a memoria allocata per i numeri di porta dei peers che mi hanno mandato un register
	time_t i_t, begin_t, end_t;	// Variabile di scorrimento delle date, inizio e fine del period
	struct tm* ptmi;			// Puntatore di scorrimento di strutture tm
	char file[BUF_LEN];
	char buff[BUF_LEN];
	char c[BUF_LEN];
	char *line;					// Puntatore a riga del file
	size_t linelen;				// Dimensione riga file
	FILE *fptr;					// Puntatore a file

	ret = 0;
	i = 0;
	num_peers = 0;
	memset(file, 0, BUF_LEN);

	// recupero inizio e fine periodo
	begin_t = mktime(&ptm_arr[0]);
	end_t = mktime(&ptm_arr[1]);

	// Se esiste la sotto-directory aggr..
	if (make_dir(port, "aggr")) {
		// ..cerco il dato aggregato che mi interessa nella sotto-directory aggr
		sprintf(file, "./%d/aggr/%s %s %4d:%02d:%02d-%4d:%02d:%02d", port, aggr, type, 
				ptm_arr[0].tm_year + 1900, ptm_arr[0].tm_mon + 1, ptm_arr[0].tm_mday, 
				ptm_arr[1].tm_year + 1900, ptm_arr[1].tm_mon + 1, ptm_arr[1].tm_mday);
		fptr = fopen(file, "r");
		// Se ho gia' calcolato il dato aggregato, lo stampo a video
		if (fptr != NULL) {
			while (getline(&line, &linelen, fptr) != -1) {
				line[strcspn(line, "\n")] = 0;
				printf("%s\n", line);
			}
			fclose(fptr);
			ret = 1;
			return ret;
		}
	}	

	// Se invece non ho gia' calcolato il dato aggregato, lo faccio adesso
	make_dir(port, "quantities");	// se non esiste la sotto-directory quantities, creala
	make_dir(port, "registers");	// se non esiste la sotto-directory registers, creala

	// Innanzitutto, se non le ho gia' mi trovo le quantities giornaliere (servono sia nella aggregazione del totale che della variazione)
	if (calc_quantities != 0) {
		// cerco riga per riga il numero di porta dei peer da cui ho ricevuto un register
		sprintf(file, "./%d/PeersList", port);
		fptr = fopen(file, "r");
		if (fptr == NULL) {
			return 0;
		}
		// calcolo il numero di peers da cui ho ricevuto registers e alloco memoria sufficiente a contenerne i numeri di porta
		while (getline(&line, &linelen, fptr) != -1) {
			line[strcspn(line, "\n")] = 0;
			num_peers++;
		}
		peer = (int*)malloc(num_peers * sizeof(int));
		// salvo i numeri di porta nella memoria allocata
		fseek(fptr, 0, SEEK_SET);
		while (getline(&line, &linelen, fptr) != -1) {
			line[strcspn(line, "\n")] = 0;
			*(peer + i) = atoi(line);
			i++;
		}
		fclose(fptr);

		// Mi procuro i dati necessari di ogni giorno del period
		for (i_t = begin_t; i_t <= end_t; i_t += 86400) {
			ptmi = localtime(&i_t); // puntatore al giorno i_t
			count = 0;				// azzero il conteggio delle quantities nel giorno i_t

			// Se esiste gia' la quantity del giorno i_t, passa al giorno successivo
			sprintf(file, "./%d/quantities/%s %4d:%02d:%02d",port, type, 
					ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
			fptr = fopen(file, "r");
			if (fptr != NULL) {
				fclose(fptr);
				continue;
			}
	
			// Conteggio delle quantities del type {n,t} di tutti i peer nel giorno i_t
			for (i = 0; i < num_peers; i++) {
				sprintf(file, "./%d/registers/%d %4d:%02d:%02d", port, *(peer + i),
						ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
				fptr = fopen(file, "r");
				if (fptr != NULL) {
					printf("Trovato register: %s \n", file);
					// scorro una entry alla volta
					memset(buff, 0, BUF_LEN);
					while(!feof(fptr)) {
						memset(c, 0, BUF_LEN);
						fread(&c, 1, 1, fptr);
						strcat(buff, c);
						if (strcmp(c, ";") == 0) {
							// prendo le quantity del type richiesto e le sommo al count del giorno i_t
							if (buff[11] == *type) {
								memset(file, 0, BUF_LEN); 
								line = buff + 13; 
								strncpy(file, line, (strlen(buff) - 14));
								printf("\t Entry: %d\n", atoi(file));
								count += atoi(file);
							}
							memset(buff, 0, BUF_LEN);
						}
					}
					printf(" Totale giornata: %d\n", count);
					fclose(fptr);
				}
			}
		
			// Scrivi nel file delle quantities il risultato ottenuto
			sprintf(file, "./%d/quantities/%s %4d:%02d:%02d",port, type, 
					ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
			fptr = fopen(file, "w");
			if(fptr == NULL) {
				fprintf(stderr, "fopen(): errore");
				exit(1);
			}
			fprintf(fptr, "%d", count);
			fclose(fptr);
			
		}

		// libero memoria allocata
		free(peer);
	}

	// Ora avendo sicuramente tutte le quantities necessarie, mi calcolo l'aggregato richiesto
	if (strcmp(aggr, "tot") == 0) {
		// calcolo il risultato sommando le quantities
		for (i_t = begin_t, count = 0; i_t <= end_t; i_t += 86400) {
			ptmi = localtime(&i_t);
			sprintf(file, "./%d/quantities/%s %4d:%02d:%02d",port, type, 
					ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
			fptr = fopen(file, "r");
			if (fptr != NULL) {
				memset(buff, 0, BUF_LEN);
				fscanf(fptr, "%s", buff);
				count += atoi(buff);
				fclose(fptr);
			}
		}
		// salvo il risultato in un file
		sprintf(file, "./%d/aggr/%s %s %4d:%02d:%02d-%4d:%02d:%02d", port, aggr, type, 
				ptm_arr[0].tm_year + 1900, ptm_arr[0].tm_mon + 1, ptm_arr[0].tm_mday, 
				ptm_arr[1].tm_year + 1900, ptm_arr[1].tm_mon + 1, ptm_arr[1].tm_mday);
		fptr = fopen(file, "w");
		if (fptr == NULL) {
			fprintf(stderr, "fopen(): errore apertura file\n");
			exit(1);
		}
		fprintf(fptr, "%d\n", count);
		fclose(fptr);
		// stampo a video il risultato
		printf("\nTOTALE: %d\n", count);
	}
	else if (strcmp(aggr, "var") == 0) {
		for (i_t = begin_t, count = 0; i_t <= end_t; i_t += 86400) {
			ptmi = localtime(&i_t);
			// leggo una quantity alla volta e sottraggo la quantity precedente
			sprintf(file, "./%d/quantities/%s %4d:%02d:%02d",port, type, 
					ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
			fptr = fopen(file, "r");
			if (fptr != NULL) {
				memset(buff, 0, BUF_LEN);
				fscanf(fptr, "%s", buff);
				count = atoi(buff) - count;
				fclose(fptr);
			}
			// salvo il risultato in un file
			sprintf(file, "./%d/aggr/%s %s %4d:%02d:%02d-%4d:%02d:%02d", port, aggr, type, 
					ptm_arr[0].tm_year + 1900, ptm_arr[0].tm_mon + 1, ptm_arr[0].tm_mday, 
					ptm_arr[1].tm_year + 1900, ptm_arr[1].tm_mon + 1, ptm_arr[1].tm_mday);
			fptr = fopen(file, "a");
			if (fptr == NULL) {
				fprintf(stderr, "fopen(): errore apertura file\n");
				exit(1);
			}
			fprintf(fptr, "[%4d:%02d:%02d] %d\n", ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday, count);
			fclose(fptr);
			// count prende il valore dell'ultima quantity letta
			count = atoi(buff);
		}
		// stampo a video il risultato
		fptr = fopen(file, "r");
		if (fptr != NULL) {
			printf("\nVARIAZIONE:\n", buff);
			while (getline(&line, &linelen, fptr) != -1) {
				line[strcspn(line, "\n")] = 0;
				printf("%s\n", line);
			}
			fclose(fptr);
		}
	}
	return ret;
}


/* Invia richiesta di flooding */
int send_flooding(int sd, int port, msg_size flood_count, msg_size count_subper, struct subperiod *subper_list) {
	uint16_t tmps;
	uint32_t tmpl;
	int ret, j;

	// Invia messaggio di richiesta FLOOD_FOR_ENTRIES al neighbor connesso sul socket sd
	ret = send(sd, FLOOD_FOR_ENTRIES, MSG_LEN, 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio FLOOD_FOR_ENTRIES\n");
		exit(1);
	}
	printf("Inviato messaggio di FLOOD_FOR_ENTRIES\n");

	// Invia numero di porta del requester
	tmps = htons(port);
	ret = send(sd, (void*)&tmps, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio numero di porta del requester\n");
		exit(1);
	}
	printf("Inviato numero di porta del requester: %d\n", port);

	// Invia counter hops del flooding
	tmps = htons(flood_count);
	ret = send(sd, (void*)&tmps, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio numero di hops\n");
		exit(1);
	}
	printf("Inviato numero di hops: %d\n", flood_count);

	// Invia numero di sottoperiodi
	tmps = htons(count_subper);
	ret = send(sd, (void*)&tmps, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio numero di sottoperiodi\n");
		exit(1);
	}

	// Invia sottoperiodi
	for (j = 0; j < count_subper; j++) {
		// invio inizio sottoperiodo
		tmpl = htonl((subper_list + j)->begin);
		ret = send(sd, (void*)&tmpl, sizeof(uint32_t), 0);
		if (ret < 0) {
			fprintf(stderr, "send(): errore invio inizio sottoperiodo %d\n", j);
			exit(1);
		}
		// invio giorni sottoperiodo
		tmps = htons((subper_list + j)->days);
		ret = send(sd, (void*)&tmps, sizeof(uint16_t), 0);
		if (ret < 0) {
			fprintf(stderr, "send(): errore invio giorni sottoperiodo %d\n", j);
			exit(1);
		}
		printf("Invio periodo: inizio %d, giorni %d\n\n", (subper_list + j)->begin, (subper_list + j)->days);
	}
}


/* Manda verso il requester la risposta al flooding */
int reply_flooding(int my_port, msg_size req_port, int *sd_neighbors, int *sd_new, struct sockaddr_in *new_addr, struct sockaddr_in *neighbors) {
	int ret;
	msg_size tmps;

	// informa il neighbor[i] che informera' a ritroso i neighbor[i] fino ad arrivare al requester
	ret = send(*sd_neighbors, REPLY_FLOOD, MSG_LEN, 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio REPLY_FLOOD: %s\n", strerror(errno));
		exit(1);
	}
	printf("Inviato REPLY_FLOOD\n");
	// mando numero di porta del requester
	tmps = htons(req_port);
	ret = send(*sd_neighbors, (void*)&tmps, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio numero di porta requester\n");
		exit(1);
	}
	printf("Inviato numero di porta requester: %d\n", req_port);
	// mando numero di porta del replier
	tmps = htons(my_port);
	ret = send(*sd_neighbors, (void*)&tmps, sizeof(msg_size), 0);
	if (ret < 0) {
		fprintf(stderr, "send(): errore invio numero di porta replier\n");
		exit(1);
	}
	printf("Inviato numero di porta replier: %d\n", my_port);

	// aspetto che il requester accetti la richiesta di connessione tcp
	if (htons(req_port) == neighbors->sin_port) {
		// se sono gia' neighbor con il requester, non devo richiedere connessione tcp
		*sd_new = *sd_neighbors;
		*new_addr = *neighbors;
		printf("Connessione TCP gia' stabilita: il requester e' il neighbor %d\n\n", req_port);
	} else {
		// creazione socket tcp
		*sd_new = socket(AF_INET, SOCK_STREAM, 0);										 
		// crezione indirizzo requester
		memset(new_addr, 0, sizeof(*new_addr));
		new_addr->sin_family = AF_INET;
		new_addr->sin_port = htons(req_port);
		ret = inet_pton(AF_INET, "127.0.0.1", &new_addr->sin_addr);
		// richiesta di connessione tcp (bloccante) al requester
		ret = connect(*sd_new, (struct sockaddr*)new_addr, sizeof(*new_addr));
		if (ret < 0) {
			fprintf(stderr, "Errore: connect() fallita\n");
			return 0;
		}
		printf("Stabilita connessione TCP con requester %d\n\n", ntohs(new_addr->sin_port));
	}

}


/* Effettua richiesta di elaborazione per ottenere il dato aggregato */
int get(int port, int sd_srv, int* sd_neighbors, char* aggr, char* type, char* period, 
		struct tm tm_arr[2], msg_size* flood_count, struct sockaddr_in* neighbors){
	int ret;
	int i, j, len;					// Variabili di scorrimento
	uint16_t tmps;					// Variabile temporanea per interi su 16bit
	uint32_t tmpl;					// Variabile temporanea per interi su 32bit
	msg_size count_subper;			// Contatore sottoperiodi di cui non abbiamo dati sufficienti
	msg_size lmsg;					// Dimensione messaggio trasmesso/ricevuto
	msg_size msglen[MAX_NEIGHBORS];	// Array per salvare le dimensioni dei messaggi ricevuti dai neighbors
	char buff[BUF_LEN];				// Buffer
	char file[BUF_LEN];				// Buffer per gestione file
	struct subperiod *subper_list;	// Puntatore a memoria allocata per la lista dei sottoperiodi mancanti
	char date[2][10];				// Buffer in cui salvare gli estremi del period
	char year[BUF_LEN], month[BUF_LEN], day[BUF_LEN];	// Buffer di appoggio per recuperare anno mese e giorno di una data formattata
	struct tm *ptm;					// Puntatore a struttura tm
	time_t begin_t, end_t;			// Data di inizio period, data di fine period
	FILE *fptr;						// Puntatore a file

	ret = -1;

	// Pulisco i buffer
	memset(year, 0, BUF_LEN);
	memset(month, 0, BUF_LEN);
	memset(day, 0, BUF_LEN);
	for (i = 0; i < 2; i++) {
		memset(date[i], 0, 10);
	}
	// Si recuperano le date dal buffer period
	for (i = 0, j = 0, len = 0; i < 21 && j < 2; i++){
		// Se la seconda data e' '*', esco dal loop
		if (j == 1 && period[i] == '*') {
			date[j][len] = period[i];
			break;
		}
		// Finche' non trovo il carattere '-', continuo a recuperare la prima data.
		// Se supero i 10 caratteri, ho recuperato la seconda data. Il prossimo j++ ci fara' uscire dal loop
		if (period[i] != '-' && len < 10) {
			date[j][len] = period[i];
			len++;	// conteggio dei caratteri in date[j] (sono esclusi '\n' e '\0')
			continue;
		}
		len = 0;
		j++;	
	}

	// se la prima data e' "*", prendi la data meno recente in assoluto
	tm_arr[0].tm_year = 2019 - 1900;
	tm_arr[0].tm_mon = 12 - 1;
	tm_arr[0].tm_mday = 2;
	tm_arr[0].tm_hour = 0;
	tm_arr[0].tm_min = 0;
	tm_arr[0].tm_sec = 0;
	tm_arr[0].tm_isdst = 0;

	begin_t = mktime(&tm_arr[0]);
	ptm = gmtime(&begin_t);
	tm_arr[0] = *ptm;

	// se la seconda data e' "*", prendi la data piu' recente in assoluto
	time(&end_t);
	ptm = localtime(&end_t);
	tm_arr[1] = *ptm;

	// aggiusta end_t per prendere solo il periodo dei registers chiusi
	if (tm_arr[1].tm_hour < 18 && tm_arr[1].tm_hour >= 0) {
		end_t -= 86400; // sottrai numero di secondi in un giorno
		ptm = gmtime(&end_t);
		tm_arr[1] = *ptm;
	}

	// per semplicita', prendo la mezzanotte come riferimento
	for (i = 0; i < 2; i++) {
		tm_arr[i].tm_hour = 0;
		tm_arr[i].tm_min = 0;
		tm_arr[i].tm_sec = 0;

		// se la data e' diversa da "*", recupero anno mese e giorno
		if (strcmp(date[i], "*") != 0) {
			for (j = 0, len = 0; j < 10 && len < 4; j++) {
				if (date[i][j] != ':') {
					if (j < 2) {
						day[len] = date[i][j];
					}
					else if (j < 5) {
						month[len] = date[i][j];
					} else if (j < 10){
						year[len] = date[i][j];
					}
					len++;
					continue;
				}
				len = 0;
			}
			printf("date[%d] %s:%s:%s \n", i, day, month, year);
			tm_arr[i].tm_isdst = 0; // disabilito l'ora legale, altrimenti mktime() nella check_period crea problemi di gestione delle date
			tm_arr[i].tm_year = atoi(year) - 1900;	// anni dal 1900
			tm_arr[i].tm_mon = atoi(month) - 1;		// mese: 0-11
			tm_arr[i].tm_mday = atoi(day);			// giorno del mese: 1-31
		}
		else {
			printf("date[%d] *\n", i);
		}
	}

	// Controllo quali entry mi mancano per calcolare l'aggregato e le metto in subper_list
	ret = check_period(port, aggr, type, tm_arr, &subper_list);
	count_subper = ret;

	// Se riscontro un errore, esci
	if (ret < 0) {
		return ret;
	}

	// Se mi manca qualche dato, lo recupero dagli altri peer della rete
	if (ret > 0) {
		printf("Dati insufficienti\n");

		// Domando ai neighbors connessi
		for (i = 0; i < MAX_NEIGHBORS; i++) {

			msglen[i] = 0;	// inizializzo msglen[i] a 0

			if (sd_neighbors[i] != -1) {
				// invia messaggio di richiesta REQ_DATA ai neighbors
				ret = send(sd_neighbors[i], REQ_DATA, MSG_LEN, 0);
				if (ret < 0) {
					fprintf(stderr, "send(): errore invio REQ_DATA\n");
					exit(1);
				}
				printf("Inviato messaggio REQ_DATA al neighbor[%d]\n", i);

				// invia richiesta di elaborazione
				memset(buff, 0, BUF_LEN);
				sprintf(buff, "%s %s %4d:%02d:%02d-%4d:%02d:%02d", aggr, type, 
						tm_arr[0].tm_year + 1900, tm_arr[0].tm_mon + 1, tm_arr[0].tm_mday, 
						tm_arr[1].tm_year + 1900, tm_arr[1].tm_mon + 1, tm_arr[1].tm_mday);
				ret = send(sd_neighbors[i], buff, strlen(buff), 0); //strlen(buff) == 27
				if (ret < 0) {
					fprintf(stderr, "send(): errore invio richiesta di elaborazione al neighbor %d\n", i);
					exit(1);
				}
				printf("Inviata richiesta di elaborazione al neighbor: %s\n", buff);

				// ricevo la dimensione del file aggr
				ret = recv(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
				if (ret < 0) {
					fprintf(stderr, "\nErrore nella recv: %s\n", strerror(errno));
					exit(1);
				}
				msglen[i] = ntohs(lmsg);
				if (msglen[i] == 0) {
					printf("Il neighbor non ha il file aggr\n");
					continue;
				}

				// apro il file aggr in scrittura concatenata
				make_dir(port, "aggr");
				memset(file, 0, BUF_LEN);
				sprintf(file, "./%d/aggr/%s %s %4d:%02d:%02d-%4d:%02d:%02d", port, aggr, type, 
						tm_arr[0].tm_year + 1900, tm_arr[0].tm_mon + 1, tm_arr[0].tm_mday, 
						tm_arr[1].tm_year + 1900, tm_arr[1].tm_mon + 1, tm_arr[1].tm_mday);
				fptr = fopen(file, "a");
				if (fptr == NULL) {
					fprintf(stderr, "fopen(): errore apertura file\n");
					exit(1);
				}

				printf("Sto per ricevere %d dati aggregati\n", msglen[i]);
				for (j = 0; j < msglen[i]; j++) {						
					
					// ricevo la dimensione del dato aggregato
					ret = recv(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
					if (ret < 0) {
						fprintf(stderr, "\nErrore nella recv: %s\n", strerror(errno));
						exit(1);
					}
					tmps = ntohs(lmsg);
					printf("Ricevuta dimensione dato aggregato (%dbytes)\n", tmps);

					// ricevo il contenuto del dato aggregato
					memset(buff, 0, BUF_LEN);
					ret = recv(sd_neighbors[i], buff, tmps, 0);
					if (ret < 0) {
						fprintf(stderr, "\nErrore nella recv: %s\n", strerror(errno));
						exit(1);
					}
					printf("Ricevuto dato aggregato: %s\n", buff);

					// salvo il dato aggregato in fondo al file aggr
					fprintf(fptr, "%s\n", buff);
				}

				// se il neighbor ha il dato aggregato, restituisco risultato
				fclose(fptr);				
				calc_aggr(port, aggr, type, tm_arr, 0);
				break;

			}

		}

		// Effettuo un flooding
		if (msglen[0] == 0 && msglen[1] == 0) {
			// mi assicuro di non avere niente nell' input buffer del socket UDP
			do {
				ret = recv(sd_srv, buff, BUF_LEN, 0);
			} while (ret >= 0);

			// chiedo numero di peers connessi al DS
			printf("Chiedo al DS il numero di peers connessi alla rete\n");
			do {
				ret = send(sd_srv, RPN_MSG, MSG_LEN, 0);
				if (ret < 0) {
					sleep(POLLING_TIME);
				}
			} while (ret < 0);
			// ricevo numero di peers connessi al DS
			do {
				ret = recv(sd_srv, (void*)&tmps, sizeof(uint16_t), 0);
			} while (ret < 0);
			tmps = ntohs(tmps);
			printf("Ricevuto numero di peers connessi alla rete: %d\n\n", tmps);

			for (i = 0; i < MAX_NEIGHBORS; i++) {

				if (sd_neighbors[i] != -1) {
					// calcolo il counter di hops da passare al neighbor i-esimo
					if (tmps == 2) {
						flood_count[i] = 1;
					} else if ((tmps % 2) == 1) {
						flood_count[i] = (tmps / 2);
					} else if (tmps > 2){
						flood_count[0] = (tmps / 2);
						flood_count[1] = (tmps / 2) - 1;
					}

					// richiesta di flooding al neighbor i-esimo
					send_flooding(sd_neighbors[i], port, flood_count[i], count_subper, subper_list);
				}

			}
		}

		free(subper_list);
		ret = 1;
	}

	// Se ho i dati, restituisco direttamente l'aggregato calcolato
	else if (ret == 0) {
		printf("Dati sufficienti\n");
		calc_aggr(port, aggr, type, tm_arr, 0);
	}

	return ret;
}


/* Richiede disconnessione dal network e manda tutte le entry registrate dall'avvio ai neighbors */
int stop(int port, struct sockaddr_in* neighbors, int* sd_neighbors, int sd_srv) {
	int i, ret;
	msg_size lmsg;
	time_t begin_t, end_t, i_t;
	struct tm *ptmb;			// Puntatore a struttura tm di begin_t
	struct tm *ptme;			// Puntatore a struttura tm di end_t
	struct tm *ptmi;			// Puntatore a struttura tm di i_t
	FILE *fptr;					// Puntatore a file
	char file[BUF_LEN];			// Nome file
	char buff[BUF_LEN];			// Buffer
	/*	NB:	Supponendo che ogni entry occupi 15~17 byte --> 1024/17 ~= 60 numero max di entry
			Numero verosimilmente piu' che sufficiente di entry in un giorno, possiamo dunque pensare di spedire il file intero */

	// Recupero l'ora attuale
	time(&end_t);

	ptme = localtime(&end_t);
		// se il register giornaliero non e' ancora stato chiuso, fermati al register di ieri
	if (ptme->tm_hour >= 0 && ptme->tm_hour < 18) {
		end_t -= 86400; // sottrai numero di secondi in un giorno
		ptme = localtime(&end_t);
	}

	// Se non esiste gia', creo la directory relativa al peer
	make_dir(port, "registers");

	// Recupero la data e l'ora dell'ultimo register inviato in fase di disconnessione ai neighbors
	sprintf(file, "./%d/StopLog", port);
	fptr = fopen(file, "r");
	if(fptr == NULL) {
		// se non esiste il file, crealo
		fptr = fopen(file, "w");
		if(fptr == NULL) {
			fprintf(stderr, "fopen(): errore");
			exit(1);
		}
		begin_t = 0;
		fprintf(fptr, "%d", begin_t);
		fclose(fptr);
	} else {
		// se esiste il file, leggi qual'e' l'ultimo registro inviato in fase di disconnessione ai neighbors
		fscanf(fptr, "%s", buff);
		begin_t = atoi(buff); 
		fclose(fptr);
	}

	ptmb = localtime(&begin_t);

 	// Per semplificare la gestione delle date, prendo come riferimento la mezzanotte
	ret = ptmb->tm_hour * 3600 + ptmb->tm_min * 60 + ptmb->tm_sec;
	begin_t -= ret;
	if (begin_t < 0) {
		begin_t = 0;
	}

	// Scorro i neighbors
	for (i = 0; i < MAX_NEIGHBORS; i++) {
		// se esiste un socket connesso ad un neighbor, invia i register chiusi successivi all'ultimo inviato
		if (sd_neighbors[i] != -1) {
			// scorro i register successivi all'ultimo inviato
			for (i_t = begin_t; i_t <= end_t; i_t += 86400) {
				ptmi = localtime(&i_t);
				sprintf(file, "./%d/registers/%d %4d:%02d:%02d", port, port, ptmi->tm_year + 1900, ptmi->tm_mon + 1, ptmi->tm_mday);
				fptr = fopen(file, "r");
				// se non esiste il register relativo a i_t, continua a ciclare..
				if (fptr == NULL) {
					continue;
				}
				// ..altrimenti copiane il contenuto in buff
				memset(buff, 0, BUF_LEN); // Pulizia
				fscanf(fptr, "%s", buff);
				fclose(fptr);
				// invia register al neighbor i-esimo
				send_register(port, sd_neighbors[i], port, ptmi, buff);
			}
			// Informo il neighbors[i] di starmi per disconnettere
			ret = send(sd_neighbors[i], DNG_MSG, MSG_LEN, 0);
			if (ret < 0) {
				fprintf(stderr, "send(): errore invio DNG_MSG\n");
				exit(1);
			}
			// Gli passo l'altro neighbor (il neighbor successivo sara' il successivo del precedente e viceversa)
			ret = (i + MAX_NEIGHBORS - 1) % MAX_NEIGHBORS;	
			lmsg = neighbors[ret].sin_port;
			ret = send(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
			if (ret < 0) {
				fprintf(stderr, "send(): errore invio neighbor\n");
				exit(1);
			}
			// Chiudi il socket ds_neighbors[i]
			close(sd_neighbors[i]);
		}
	}

	// Scrivi nel file di log qual'e' l'ultimo register inviato ai neighbors in fase di disconnessione
	sprintf(file, "./%d/StopLog", port);
	fptr = fopen(file, "w");
	if(fptr == NULL) {
		fprintf(stderr, "fopen(): errore\n");
		exit(1);
	}
	fprintf(fptr, "%d", end_t);
	fclose(fptr);

	// Manda al DS la richiesta di disconnessione dal network
	printf("Invio richiesta di disconnessione al DS...\n");
	do {
		ret = send(sd_srv, DNG_MSG, MSG_LEN, 0);
		if (ret < 0) {
			fprintf(stderr, "send(): errore invio DNG_MSG al DS\n");
			sleep(POLLING_TIME);
		}
	} while (ret < 0);
	printf("Inviato DNG_MSG al DS\n");
}


/******************************************************* MAIN() *******************************************************/
int main(int argc, char* argv[]){

	int my_port;										// Porta associata al peer
	int i, j, k;										// Variabili di scorrimento
	int ret;											// Variabile per il controllo dei valori di ritorno delle funzioni
	int len, addrlen;									// Lunghezze di buffer e indirizzi
	msg_size lmsg, req_port;							// Lunghezza messaggi e num. di porta del requester, entrambi ricevuti con dimensione uint16_t
	msg_size flood_count[MAX_NEIGHBORS];				// Counter hops del flooding: [0] antiorario, [1] orario
	msg_size hops;										// Counter da inoltrare agli altri peers durante il flooding
	uint16_t tmps;										// Variabile temporanea per interi su 16bit
	uint32_t tmpl;										// Variabile temporanea per interi su 32bit
	regex_t regex;										// Espressione regolare
	char buffer[BUF_LEN];								// Buffer generico
	char file[BUF_LEN];									// Buffer per gestione file
	FILE *fptr;											// Puntatore a file

	int *peer_port;										// Puntatore a memoria allocata per i numeri di porta dei peers che mi hanno mandato un register
	int num_peers;										// Contatore peers che mi hanno mandato un register
	char *line = NULL;									// Puntatore a carattere per accedere riga per riga ad un file
	size_t linelen;										// Dimensione riga

	int found;											// 0: non ho trovato registers nel sottoperiodo, 1: ho trovato registers nel sottoperiodo
	struct subperiod *subper_tmp;						// Puntatore a memoria allocata per la lista dei sottoperiodi richiesti da un flooding
	time_t begin, end;									// Variabili usate per scorrere il sottoperiodo
	char tmp_aggr[BUF_LEN];								// aggr della ultima get()
	char tmp_type[BUF_LEN];								// type della ultima get()
	struct tm tm_array[2];								// Estremi dell'ultimo period richiesto con la get(): [0] inizio, [1] fine
	struct tm *ptm;										// Puntatore a tm, usato per scorrere le date nel sottoperiodo

	fd_set master;										// Set di descrittori da monitorare in lettura
	fd_set read_fds;									// Set di descrittori pronti in lettura
	int fdmax;											// Descrittore max
	int fd_stdin;										// Descrittore stdin
	char cmd[MAX_CMD_ARG][BUF_LEN];						// cmd[0]: nome del comando;   cmd[>0] valori degli argomenti

	int sd_srv;											// Descrittore socket UDP per il DS
	int listener;										// Descrittore listener socket
	int sd_new;											// Descrittore socket TCP per peer non-neighbor
	int sd_neighbors[MAX_NEIGHBORS];					// Descrittore socket TCP per i neighbors
	struct sockaddr_in my_addr, srv_addr;				// Strutture per gli indirizzi del peer e del DS
	struct sockaddr_in neighbors[MAX_NEIGHBORS];		// Struttura per gli indirizzi dei peer neighbor
	int tmp_neigh[MAX_NEIGHBORS];						// Struttura per controllare i neighbor che stiamo sostituendo dopo una recv_list()
	struct sockaddr_in new_addr;						// Struttura per gli indirizzi dei peer non-neighbor con cui si stabilisce una connessione momentanea

	/* Acquisizione della porta associata al peer */
	 // Il peer si avvia con il comando	./peer <porta>
	 // dunque argv[1] punta alla stringa contenente la porta associata al peer
	if (argv[1] == NULL) {
		fprintf(stderr, "Nessuna porta associata al peer\n");
		exit(1);
	}
	ret = atoi(argv[1]);	// atoi() converte una stringa in un intero
	if (ret <= 0) {
		fprintf(stderr, "Porta peer non valida\n");
		exit(1);
	}
	my_port = ret;

	/* Se non esiste gia', creo la directory relativa al peer */
	make_dir(my_port, "registers");

	/* Se ancora non esiste il file PeersList, lo creo e scrivo il mio numero di porta */
	memset(file, 0, BUF_LEN);
	sprintf(file, "./%d/PeersList", my_port);
	fptr = fopen(file, "r");
	if (fptr == NULL) {
		fptr = fopen(file, "w");
		if (fptr == NULL) {
			fprintf(stderr, "fopen(): errore");
			exit(1);
		}
		fprintf(fptr, "%d\n", my_port);
	}
	fclose(fptr);

	/* File Descriptor dello standard input */
	fd_stdin = fileno(stdin);

	/* Pulizia indirizzi */
	memset(&my_addr, 0, sizeof(my_addr));
	memset(&srv_addr, 0, sizeof(srv_addr));
	memset(&new_addr, 0, sizeof(new_addr));
	for (i = 0; i < MAX_NEIGHBORS; i++) {
		memset(&neighbors[i], 0, sizeof(neighbors[i]));
		tmp_neigh[i] = 0;
		flood_count[i] = 0;
		sd_neighbors[i] = -1;	//poiche' non vengono assegnati valori negativi ai file descriptor, usiamo -1 come valore di "non-assegnamento"
	}

	/* Creazione indirizzo di bind */
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(my_port);
	my_addr.sin_addr.s_addr = INADDR_ANY;


	/* Creazione socket UDP non bloccante */
	sd_srv = socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK, 0);
	/* Aggancio del socket UDP all'indirizzo di bind */
	ret = bind(sd_srv, (struct sockaddr*)&my_addr, sizeof(my_addr));
	if (ret < 0) {
		fprintf(stderr, "\nBind sd_srv non riuscita: %s\n", strerror(errno));
		exit(1);
	}

	/* Creazione listener socket */
	listener = socket(AF_INET, SOCK_STREAM, 0);
	/* Aggancio del listener socket all'indirizzo di bind */
	ret = bind(listener, (struct sockaddr*)&my_addr, sizeof(my_addr));
	if (ret < 0) {
		fprintf(stderr, "\nBind listener non riuscita: %s\n", strerror(errno));
		exit(1);
	}
	/* Apro la coda */
	listen(listener, 10);

	// Reset dei descrittori
	FD_ZERO(&master);
	FD_ZERO(&read_fds);

	// Aggiungo stdin e listener socket ai descrittori monitorati
	FD_SET(fd_stdin, &master);
	FD_SET(sd_srv, &master);
	FD_SET(listener, &master);

	// Tengo traccia del nuovo fdmax
	if (fd_stdin > listener) {
		fdmax = fd_stdin;
	} else {
		fdmax = listener;
	}
	if (sd_srv > fdmax) {
		fdmax = sd_srv;
	}

	/* Stampa la schermata di avvio */
	start_screen(my_port);

	/* Se il DS e' stato passato come secondo argomento, effettuo il boot del peer */
	if (argc > 2) {
		ret = atoi(argv[2]);
		if (start(sd_srv, &srv_addr, "127.0.0.1", ret, neighbors) == 1) {
			for (i = 0; i < MAX_NEIGHBORS; i++) {
				// Tento una connessione TCP con ogni neighbor
				if (neighbors[i].sin_port != 0) {
					// creo socket connesso con il neighbor i-esimo
					sd_neighbors[i] = socket(AF_INET, SOCK_STREAM, 0);
					// aggiungo il descrittore del socket al set da controllare in lettura
					FD_SET(sd_neighbors[i], &master);
					if (sd_neighbors[i] > fdmax) {
						fdmax = sd_neighbors[i];
					}
					// richiesta di connessione TCP al neighbor i-esimo
					printf("Richiesta connessione TCP con neighbor %d...\n", ntohs(neighbors[i].sin_port));
					ret = connect(sd_neighbors[i], (struct sockaddr*)&neighbors[i], sizeof(neighbors[i]));
					if (ret < 0) {
						fprintf(stderr, "\nErrore nella connect: %s\n", strerror(errno));
						exit(1);
					} else {
						printf("Accettata\n");
					}
				}
				// salva nella struttura temporanea i neighbors che sostituiremo alla prossima recv_list()
				tmp_neigh[i] = neighbors[i].sin_port;
			}
		}
		printf("\n");
	}


	/* Ciclo infinito */
	while(1) {

		read_fds = master;

		// Pulizia buffer
		memset(buffer, 0, BUF_LEN);
		for (i = 0; i < MAX_CMD_ARG; i++)
			memset(cmd[i], 0, BUF_LEN);

		/* Utilizzo la select() per rendere non-bloccante la lettura dallo stdin e per controllare piu' socket*/
		ret = select(fdmax + 1, &read_fds, NULL, NULL, NULL);
		if (ret == -1) {
			fprintf(stderr, "\nErrore nella select: %s\n", strerror(errno));
			exit(1);
		} 

		/* Se qualcosa e' stato scritto nello standard input, controlliamo che sia uno dei comandi accettati dal peer */
		if (FD_ISSET(fd_stdin, &read_fds)) {
			ret = read(fd_stdin, buffer, BUF_LEN);			// read() non conteggia '\0', ma comunque rimane '\n'
			if (ret < 0) {
				fprintf(stderr, "\nErrore nella read: %s\n", strerror(errno));
				exit(1);
			}

			// Comando stop
			if (strcmp(buffer, "stop\n\0") == 0) {
				stop(my_port, neighbors, sd_neighbors, sd_srv);
				break;
			}

			// Si recuperano gli argomenti con cui e' stato chiamato il comando da tastiera
			for (i = 0, j = 0, len = 0; i < ret && j < MAX_CMD_ARG; i++){
				if (buffer[i] != ' ' && buffer[i] != '\n'){
					cmd[j][len] = buffer[i];
					len++;	// conteggio dei caratteri in cmd (sono esclusi '\n' e '\0')
					continue;
				}

				len = 0;
				j++;	
			}

			// Comando start <DS_addr> <DS_port>
			if (strcmp(cmd[0], "start") == 0) {
				if ( strlen(cmd[1]) == 0 || strlen(cmd[2]) == 0 || strlen(cmd[3]) != 0) {
					fprintf(stderr, "start <DS_addr> <DS_port>: chiamata non corretta\n");
					continue;
				}
				ret = atoi(cmd[2]);	// ret = <DS_port>
				if (ret <= 0) {
					fprintf(stderr, "start <DS_addr> <DS_port>: porta DS non valida\n");
					continue;
				}
				// Effettuo il boot
				if (start(sd_srv, &srv_addr, cmd[1], ret, neighbors) == 1) {
					for (i = 0; i < MAX_NEIGHBORS; i++) {
						// Tento una connessione TCP con ogni neighbor
						if (neighbors[i].sin_port != 0) {
							// creo socket connesso con il neighbor i-esimo
							sd_neighbors[i] = socket(AF_INET, SOCK_STREAM, 0);
							// aggiungo il descrittore del socket al set da controllare in lettura
							FD_SET(sd_neighbors[i], &master);
							if (sd_neighbors[i] > fdmax) {
								fdmax = sd_neighbors[i];
							}

							// richiesta di connessione TCP al neighbor i-esimo
							printf("Richiesta connessione TCP con neighbor %d...\n", ntohs(neighbors[i].sin_port));
							ret = connect(sd_neighbors[i], (struct sockaddr*)&neighbors[i], sizeof(neighbors[i]));
							if (ret < 0) {
								fprintf(stderr, "\nErrore nella connect: %s\n", strerror(errno));
								exit(1);
							} else {
								printf("Accettata\n");
							}
						}
						// salva nella struttura temporanea i neighbors che sostituiremo alla prossima recv_list()
						tmp_neigh[i] = neighbors[i].sin_port;
					}
				}
				printf("\n");
				continue;
			}

			// Comando add <type> <quantity>
			if (strcmp(cmd[0], "add") == 0) {
				if ( strlen(cmd[1]) == 0 || strlen(cmd[2]) == 0 || strlen(cmd[3]) != 0) {
					fprintf(stderr, "add <type> <quantity>: chiamata non corretta\n");
					continue;
				}
				ret = atoi(cmd[2]);
				if (ret <= 0) {
					fprintf(stderr, "add <type> <quantity>: quantita' non valida\n");
					continue;
				}
				if ( !(strcmp(cmd[1], "n") == 0 || strcmp(cmd[1], "t") == 0) ) {
					fprintf(stderr, "add <type> <quantity>: inserire uno dei type consentiti {n,t}\n");
					continue;
				}
				add(my_port, cmd[1], ret);
				continue;
			}

			// Comando get <aggr> <type> <period>
			if (strcmp(cmd[0], "get") == 0) {
				// mi assicuro che non ci sia gia' in corso un flooding richiesto da questo peer
				if (flood_count[0] != 0 || flood_count[1] != 0) {
					fprintf(stderr, "Attendere: elaborazione di una get precedente in corso..\n");
					continue;
				}
				// controllo i parametri passati
				if ( !(strcmp(cmd[1], "tot") == 0 || strcmp(cmd[1], "var") == 0) ) {
					fprintf(stderr, "get <aggr> <type> <period>: inserire uno degli aggr consentiti {tot,var}\n");
					continue;
				}
				if ( !(strcmp(cmd[2], "n") == 0 || strcmp(cmd[2], "t") == 0) ) {
					fprintf(stderr, "get <aggr> <type> <period>: inserire uno dei type consentiti {n,t}\n");
					continue;
				}
				// accetta solo period nei formati dd1:mm1:yyyy1-dd2:mm2:yyyy2   dd1:mm1:yyyy1-*   *-dd2:mm2:yyyy2   *-*
				regcomp(&regex, "^[0-3][0-9]:[0-1][0-9]:[0-9][0-9][0-9][0-9]-[0-3][0-9]:[0-1][0-9]:[0-9][0-9][0-9][0-9]$", 0);
				ret = regexec(&regex, cmd[3], 0, NULL, 0);
				if (ret != 0) {
					regcomp(&regex, "^[*]-[0-3][0-9]:[0-1][0-9]:[0-9][0-9][0-9][0-9]$", 0);
					ret = regexec(&regex, cmd[3], 0, NULL, 0);
					if (ret != 0) {
						regcomp(&regex, "^[0-3][0-9]:[0-1][0-9]:[0-9][0-9][0-9][0-9]-[*]$", 0);
						ret = regexec(&regex, cmd[3], 0, NULL, 0);
						if (ret != 0) {
							regcomp(&regex, "^[*]-[*]$", 0);
							ret = regexec(&regex, cmd[3], 0, NULL, 0);
						}
					}
				}
				if (ret != 0) {
					fprintf(stderr, "get <aggr> <type> <period>: inserire period valido `dd1:mm1:yyyy1-dd2:mm2:yyyy2`\n");
					continue;
				}

				memset(tmp_aggr, 0, BUF_LEN);
				memset(tmp_type, 0, BUF_LEN);
				strcpy(tmp_aggr, cmd[1]);
				strcpy(tmp_type, cmd[2]);

				ret = get(my_port, sd_srv, sd_neighbors, cmd[1], cmd[2], cmd[3], tm_array, flood_count, neighbors);
				if (ret < 0) {
					fprintf(stderr, "Errore get()\n");
					memset(&tm_array[0], 0, sizeof(tm_array[0]));
					memset(&tm_array[1], 0, sizeof(tm_array[1]));
				}

				continue;
			}

			fprintf(stderr, "comando sconosciuto: %s\n", buffer);
			continue;
		}

		/* Se ricevo richieste dal DS, le gestisco */
		if (FD_ISSET(sd_srv, &read_fds)) {
			ret = recv(sd_srv, buffer, MSG_LEN, 0);
			if (ret < 0) {
				fprintf(stderr, "\nErrore nella recv: %s\n", strerror(errno));
				exit(1);
			}

			// Se ricevo LST_MSG dal DS, gestisci l'aggiornamento della lista
			if (strcmp(buffer, LST_MSG) == 0) {
				printf("Richiesta dal DS: %s\n", buffer);
				ret = recv_list(sd_srv, neighbors);
				if (ret >= 0) {
					printf("Lista neighbors aggiornata\n");

					// Poiche' la lista e' circolare, dobbiamo gestire il caso in cui un neighbor successivo diventa un neighbor precedente, e viceversa
					if ((tmp_neigh[0] == neighbors[1].sin_port && tmp_neigh[0] != 0) 
					 || (tmp_neigh[1] == neighbors[0].sin_port && tmp_neigh[1] != 0)) {

						ret = sd_neighbors[1];
						sd_neighbors[1] = sd_neighbors[0];
						sd_neighbors[0] = ret;

						if (tmp_neigh[0] == neighbors[1].sin_port) {
							tmp_neigh[0] = tmp_neigh[1];
							tmp_neigh[1] = neighbors[1].sin_port;
						} else {
							tmp_neigh[1] = tmp_neigh[0];
							tmp_neigh[0] = neighbors[0].sin_port;
						}

					}

					// Accetto richieste di connessione TCP dai neighbors
					for (i = 0; i < MAX_NEIGHBORS; i++) {
						// Se un socket e' gia' stato connesso al neighbor i-esimo, ma questo e' diverso dal neighbor i-esimo precedente..
						if (sd_neighbors[i] != -1 && neighbors[i].sin_port != tmp_neigh[i]) {
							// chiudo il vecchio socket connesso sd_neighbors[i]
							close(sd_neighbors[i]);
							printf("Chiusa connessione TCP con neighbor %d\n", ntohs(tmp_neigh[i]));

							// elimino dal set dei descrittori monitorati il descrittore del vecchio socket connesso
							FD_CLR(sd_neighbors[i], &master);
							sd_neighbors[i] = -1;
						}

						// Se il nuovo neighbor i-esimo e' diverso da 0 e dal neighbor i-esimo precedente..
						if (neighbors[i].sin_port != 0 && neighbors[i].sin_port != tmp_neigh[i]) {
							// accetto la connessione e creo il socket connesso sd_neighbors[i]
							sd_neighbors[i] = accept(listener, NULL, NULL);
							printf("Accettata connessione TCP con neighbor %d\n", ntohs(neighbors[i].sin_port));
							// aggiungo il descrittore del socket connesso al set dei descrittori monitorati
							FD_SET(sd_neighbors[i], &master);

							// tengo traccia del nuovo fdmax
							if (sd_neighbors[0] > sd_neighbors[1]) {
								fdmax = sd_neighbors[0];
							} else {
								fdmax = sd_neighbors[1];
							}
							if (fd_stdin > fdmax) {
								fdmax = fd_stdin;
							}
							if (listener > fdmax){
								fdmax = listener;
							}
							if (sd_srv > fdmax) {
								fdmax = sd_srv;
							}

							// salva nella struttura temporanea i neighbors che sostituiremo alla prossima recv_list()
							tmp_neigh[i] = neighbors[i].sin_port;
						}
					}

					printf("\n");
				}

			}

			// Se ricevo ESC_MSG dal DS, termina il peer
			else if (strcmp(buffer, ESC_MSG) == 0) {
				printf("Richiesta dal DS: %s\n", buffer);
				break;
			}

			// Se ricevo una richiesta non valida, ignorala
			else {
				fprintf(stderr, "Richiesta dal DS non valida %s\n", buffer);
			}
			continue;
		}


		for (i = 0; i < MAX_NEIGHBORS; i++) {
			if (FD_ISSET(sd_neighbors[i], &read_fds) && sd_neighbors[i] != -1) {
				recv(sd_neighbors[i], (void*)buffer, MSG_LEN, 0);
				printf("Richiesta ricevuta dal neighbor %d: %s\n", ntohs(neighbors[i].sin_port), buffer);


			// Se ricevo RNG_MSG, gestisco la ricezione di un register di un neighbor
				if (strcmp(buffer, RNG_MSG) == 0) {
					recv_register(my_port, sd_neighbors[i], ntohs(neighbors[i].sin_port));
				}


			// Se ricevo DNG_MSG, gestisco la disconnessione di un neighbor
				if (strcmp(buffer, DNG_MSG) == 0) {
					// Ricevo il nuovo neighbor dal peer che si sta disconnettendo
					recv(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
					neighbors[i].sin_port = lmsg;
					tmp_neigh[i] = neighbors[i].sin_port;
					// Chiudo il socket e lo elimino dal set di descrittori controllati in lettura
					close(sd_neighbors[i]);
					FD_CLR(sd_neighbors[i], &master);
					// Se il neighbor ricevuto non c'e' o e' uguale a quello che ho gia', lo ignoro
					if (neighbors[i].sin_port == 0 || neighbors[0].sin_port == neighbors[1].sin_port) {
						// pulisco le strutture relative al neighbor i-esimo
						sd_neighbors[i] = -1;
						memset(&neighbors[i], 0, sizeof(neighbors[i]));
						tmp_neigh[i] = 0;
					} else {
						// Se si sta disconnettendo il neighbor precedente, mi connetto con un nuovo neighbor precedente
						if (i == 0) {
							// creo socket connesso con il neighbor i-esimo
							sd_neighbors[i] = socket(AF_INET, SOCK_STREAM, 0);
							// aggiungo il descrittore del socket al set da controllare in lettura
							FD_SET(sd_neighbors[i], &master);
							// richiedo connessione TCP al nuovo neighbor
							ret = connect(sd_neighbors[i], (struct sockaddr*)&neighbors[i], sizeof(neighbors[i]));
							if (ret < 0) {
								fprintf(stderr, "\nErrore nella connect: %s\n", strerror(errno));
								exit(1);
							}
							printf("Stabilita connessione TCP con neighbor %d\n", ntohs(neighbors[i].sin_port));
						}
						// Se si sta disconnettendo il neighbor successivo, mi connetto con un nuovo neighbor successivo
						if (i == 1) {
							// accetto la connessione e creo il socket connesso sd_neighbors[i]
							sd_neighbors[i] = accept(listener, NULL, NULL);
							printf("Accettata connessione TCP con neighbor %d\n", ntohs(neighbors[i].sin_port));
							// aggiungo il descrittore del socket connesso al set dei descrittori monitorati
							FD_SET(sd_neighbors[i], &master);
						}
					}
					// tengo traccia del nuovo fdmax
					if (sd_neighbors[0] > sd_neighbors[1]) {
						fdmax = sd_neighbors[0];
					} else {
						fdmax = sd_neighbors[1];
					}
					if (fd_stdin > fdmax) {
						fdmax = fd_stdin;
					}
					if (listener > fdmax){
						fdmax = listener;
					}
					if (sd_srv > fdmax) {
						fdmax = sd_srv;
					}
					printf("\n");
				}


			// Se ricevo REQ_DATA, gestisco la richiesta di dati aggregati gia' calcolati
				if (strcmp(buffer, REQ_DATA) == 0) {
					// Ricevo richiesta di elaborazione
					memset(buffer, 0, BUF_LEN); // Pulizia buffer
					ret = recv(sd_neighbors[i], (void*)buffer, 27, 0);	//richiesta formattata "%s %s %4d:%02d:%02d-%4d:%02d:%02d" (27 caratteri)
					printf("Richiesta di elaborazione: %s\n", buffer);
					if (ret < 0) {
						fprintf(stderr, "recv(): errore ricezione richiesta di elaborazione dal neighbor %d\n", i);
						exit(1);
					}
					// Invio dimensione file con dato aggregato: se non ce l'ho, invio 0
					make_dir(my_port, "aggr");	// controllo che esista la sotto-directory aggr
					// Cerco il dato aggregato che mi interessa nella sotto-directory
					sprintf(file, "./%d/aggr/%s", my_port, buffer);
					fptr = fopen(file, "r");
					if (fptr == NULL) {
						lmsg = 0;
						ret = send(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
						if (ret < 0) {
							fprintf(stderr, "send(): errore comunicazione di file aggr mancante\n");
							exit(1);
						}
						printf("Dato aggregato non trovato\n");
					} else {
						lmsg = 0;
						while (getline(&line, &linelen, fptr) != -1) {
							line[strcspn(line, "\n")] = 0;
							lmsg++;
						}
						// Invio dimensione del file aggr
						lmsg = htons(lmsg);
						ret = send(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
						if (ret < 0) {
							fprintf(stderr, "send(): errore invio dimensione file aggr\n");
							exit(1);
						}
						printf("Inviato numero di dati del file aggr: %d\n", lmsg);

						// Invio contenuto del file aggr
						fseek(fptr, 0, SEEK_SET);
						while (getline(&line, &linelen, fptr) != -1) {
							line[strcspn(line, "\n")] = 0;
							// Invio dimensione del dato aggr
							lmsg = htons(strlen(line));
							ret = send(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
							if (ret < 0) {
								fprintf(stderr, "send(): errore invio dimensione dato aggregato\n");
								exit(1);
							}
							printf("Inviata dimensione del dato aggregato (%dbyte)\n", strlen(line));
							// Invio contenuto del dato aggr
							ret = send(sd_neighbors[i], (void*)line, strlen(line), 0);
							if (ret < 0) {
								fprintf(stderr, "send(): errore invio file aggr\n");
								exit(1);
							}
						}

						fclose(fptr);
						printf("Dato aggregato inviato\n");
					}

				}


			// Se ricevo FLOOD_FOR_ENTRIES, gestisco la richiesta di flooding
				if (strcmp(buffer, FLOOD_FOR_ENTRIES) == 0) {
					// Ricevo info per identificare il requester (numero di porta)
					ret = recv(sd_neighbors[i], (void*)&req_port, sizeof(msg_size), 0);
					if (ret < 0) {
						fprintf(stderr, "recv(): errore ricezione numero di porta del requester\n");
						exit(1);
					}
					req_port = ntohs(req_port);
					printf("Numero di porta del requester: %d\n", req_port);
					// se mi riconosco come requester, vuol dire che il flooding ha toccato tutti i peers della rete
					// dunque posso fermare il flooding e calcolarmi l'aggregato richiesto dall'ultimo get()
					if (req_port == my_port) {
						for (j = 0; j < MAX_NEIGHBORS; j++) {
							flood_count[j] = 0;
						}
						calc_aggr(my_port, tmp_aggr, tmp_type, tm_array, 1);
						continue;
					}

					// Ricevo counter hops del flooding e lo decremento
					ret = recv(sd_neighbors[i], (void*)&tmps, sizeof(msg_size), 0);
					if (ret < 0) {
						fprintf(stderr, "recv(): errore ricezione numero di hops\n");
						exit(1);
					}
					hops = ntohs(tmps);
					printf("ricevuto numero di hops: %d\n", hops);
					hops--;

					// Ricevo numero di sottoperiodi
					ret = recv(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
					if (ret < 0) {
						fprintf(stderr, "recv(): errore ricezione  numero di sottoperiodi\n");
						exit(1);
					}
					lmsg = ntohs(lmsg);
					printf("Numero di sottoperiodi: %d\n", lmsg);

					subper_tmp = (struct subperiod*)malloc(lmsg * sizeof(struct subperiod));

					// Ricevo sottoperiodi
					for (j = 0; j < lmsg; j++) {
						// ricevo inizio sottoperiodo
						ret = recv(sd_neighbors[i], (void*)&((subper_tmp + j)->begin), sizeof(uint32_t), 0);
						if (ret < 0) {
							fprintf(stderr, "recv(): errore ricezione inizio sottoperiodo %d\n", j);
							exit(1);
						}
						(subper_tmp + j)->begin = ntohl((subper_tmp + j)->begin);
						// ricevo giorni sottoperiodo
						ret = recv(sd_neighbors[i], (void*)&((subper_tmp + j)->days), sizeof(uint16_t), 0);
						if (ret < 0) {
							fprintf(stderr, "recv(): errore ricezione giorni sottoperiodo %d\n", j);
							exit(1);
						}
						(subper_tmp + j)->days = ntohs((subper_tmp + j)->days);
						printf("Ricevuto periodo: inizio %d + %d giorni\n", (subper_tmp + j)->begin, (subper_tmp + j)->days);
					}


					// Recupero dal file PeersList il numero di porta dei peer da cui ho ricevuto un register
					// cosi' evito di ciclare i sottoperiodi per tutti i numeri di porta possibili
					memset(file, 0, BUF_LEN);
					sprintf(file, "./%d/PeersList", my_port);
					fptr = fopen(file, "r");
					num_peers = 0;
					if (fptr != NULL) {
						// calcolo il numero di peers da cui ho ricevuto registers e alloco memoria sufficiente a contenerne i numeri di porta
						while (getline(&line, &linelen, fptr) != -1) {
							line[strcspn(line, "\n")] = 0;
							num_peers++;
						}
						printf("Trovati %d peers nel file %s\n", num_peers, file);
						peer_port = (int*)malloc(num_peers * sizeof(int));
						// salvo i numeri di porta nella memoria allocata
						fseek(fptr, 0, SEEK_SET);
						k = 0;
						while (getline(&line, &linelen, fptr) != -1) {
							line[strcspn(line, "\n")] = 0;
							*(peer_port + k) = atoi(line);
							k++;
						}
						fclose(fptr);
					}

					// Cerca registers all'interno dei sottoperiodi ricevuti
					found = 0;
					for (j = 0; j < lmsg; j++) {
						begin = (subper_tmp + j)->begin;
						end = begin + 86400 * (subper_tmp + j)->days;

						while (begin <= end) {
							ptm = gmtime(&begin);
							memset(file, 0, BUF_LEN);

							for (k = 0; k < num_peers; k++) {
								sprintf(file, "./%d/registers/%d %4d:%02d:%02d",
										my_port, *(peer_port + k), ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
								fptr = fopen(file, "r");

								if (fptr != NULL) {
									printf("Trovato register: %s\n", file);
									// recupero contenuto del register
									memset(buffer, 0, BUF_LEN); // Pulizia buffer
									fscanf(fptr, "%s", buffer);
									fclose(fptr);

									// al primo register trovato, informo il requester
									if (found == 0) {
										reply_flooding(my_port, req_port, &sd_neighbors[i], &sd_new, &new_addr, &neighbors[i]);
										found = 1;										
									}

									// invia registers relativi ai sottoperiodi
									printf("Invio register: %s\n", file);
									send_register(my_port, sd_new, *(peer_port + k), ptm, buffer);
								}
							}

							begin += 86400;
						}

					}

					// Se sono l'ultimo peer del flooding, lo comunico al requester
					if (hops <= 0) {
						if (found == 0) {
							// se non mi ero gia' connesso con il requester, lo faccio ora
							reply_flooding(my_port, req_port, &sd_neighbors[i], &sd_new, &new_addr, &neighbors[i]);
							found = 1;
						}

						ret = send(sd_new, FLOOD_END, MSG_LEN, 0);
						if (ret < 0) {
							fprintf(stderr, "send(): errore invio REPLY_END al requester\n");
							exit(1);
						}
						printf("Inviato FLOOD_END al requester\n");
						printf("Flooding fermato\n");
					}
					// Altrimenti inoltro flooding al neighbor[(i + 1) % MAX_NEIGHBORS]
					else {
						if (found == 1) {
							// se stavo trasmettendo registri al requester, gli invio un messaggio di fine trasmissione
							ret = send(sd_new, REPLY_END, MSG_LEN, 0);
							if (ret < 0) {
								fprintf(stderr, "send(): errore invio REPLY_END al requester\n");
								exit(1);
							}
							printf("Inviato REPLY_END al requester\n");
						}

						printf("Inoltro flooding al peer %d\n", ntohs(neighbors[(i + 1) % MAX_NEIGHBORS].sin_port));
						send_flooding(sd_neighbors[(i + 1) % MAX_NEIGHBORS], req_port, hops, lmsg, subper_tmp);
					}

					// Disconnessione dal requester
					if (found == 1 && sd_new != sd_neighbors[i]) {
						printf("Disconnessione dal requester\n\n");
						close(sd_new);
					}

					// libero memoria allocata
					free(peer_port);
					free(subper_tmp);
				}


			// Se ricevo REPLY_FLOOD, gestisco la risposta al flooding
				if (strcmp(buffer, REPLY_FLOOD) == 0) {

					// ricevo il numero di porta del requester
					ret = recv(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
					if (ret < 0) {
						fprintf(stderr, "recv(): errore ricezione numero di porta requester\n");
						exit(1);
					}
					tmps = ntohs(lmsg);
					printf("Ricevuto numero di porta requester: %d\n", tmps);

					ret = recv(sd_neighbors[i], (void*)&lmsg, sizeof(msg_size), 0);
					if (ret < 0) {
						fprintf(stderr, "recv(): errore ricezione numero di porta replier\n");
						exit(1);
					}
					printf("Ricevuto numero di porta replier: %d\n", ntohs(lmsg));

					// se non sono il requester, inoltro all'indietro il REPLY_FLOOD
					if (my_port != tmps) {
						ret = send(sd_neighbors[(i + 1) % MAX_NEIGHBORS], REPLY_FLOOD, MSG_LEN, 0);
						if (ret < 0) {
							fprintf(stderr, "send(): errore inoltro REPLY_FLOOD: %s\n", strerror(errno));
							exit(1);
						}
						printf("Inoltrato REPLY_FLOOD\n");

						tmps = htons(tmps);
						ret = send(sd_neighbors[(i + 1) % MAX_NEIGHBORS], (void*)&tmps, sizeof(msg_size), 0);
						if (ret < 0) {
							fprintf(stderr, "send(): errore invio numero di porta requester\n");
							exit(1);
						}
						printf("Inviato numero di porta requester\n");

						ret = send(sd_neighbors[(i + 1) % MAX_NEIGHBORS], (void*)&lmsg, sizeof(msg_size), 0);
						if (ret < 0) {
							fprintf(stderr, "send(): errore invio numero di porta replier\n");
							exit(1);
						}
						printf("Inviato numero di porta replier\n");
					}

					// se sono il requester, stabilisci connessione TCP con il replier
					else {
						memset(&new_addr, 0, sizeof(new_addr));
						if (lmsg == neighbors[i].sin_port) {
							// se il replier e' il neighbor i, non devo richiedere connessione tcp
							sd_new = sd_neighbors[i];
							new_addr = neighbors[i];
							printf("Connessione TCP gia' stabilita: il replier e' il neighbor %d\n\n", ntohs(neighbors[i].sin_port));
						} else {
							sd_new = accept(listener, (struct sockaddr*)&new_addr, &addrlen);
							printf("Accettata connessione TCP con peer %d\n\n", ntohs(lmsg));
						}

						// ricevi tutti i registers che non possiedo dal replier
						while(1) {
							ret = recv(sd_new, buffer, MSG_LEN, 0);
							if (ret < 0) {
								fprintf(stderr, "recv(): errore ricezione richiesta del replier\n");
								exit(1);
							}
						// Se ricevo RNG_MSG, gestisco la ricezione di un register dal replier
							if (strcmp(buffer, RNG_MSG) == 0) {
								recv_register(my_port, sd_new, ntohs(lmsg));
							}
						// Se ricevo REPLY_END, smetto di attendere registers dal replier
							if (strcmp(buffer, REPLY_END) == 0) {
								printf("Ricevuto REPLY_END\n");
								break;
							}
						// Se ricevo FLOOD_END, smetto di attendere registers dal replier e resetto flood_count[i]
							if (strcmp(buffer, FLOOD_END) == 0) {
								printf("Ricevuto FLOOD_END\n\n");
								flood_count[i] = 0;
								break;
							}
						}
						
						// disconnessione dal replier
						if (sd_new != sd_neighbors[i]) {
							printf("Disconnessione dal replier\n\n");
							close(sd_new);
						}

						// Calcolo il risultato se flooding_count sono entrambi a 0
						if (flood_count[0] == 0 && flood_count[1] == 0) {
							calc_aggr(my_port, tmp_aggr, tmp_type, tm_array, 1);
						}
					}

				}


			}
		}

	}

	/* Chiusura del peer */	
	printf("Terminazione peer\n");
	close(sd_srv);
	close(listener);
}
