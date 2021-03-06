#include "socket.h"
#include "client.h"
#include "packet_interface.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h> /* * sockaddr_in6 */
#include <netdb.h>
#include <math.h>


void adapt_buffer(int *small_seq,int seq, int *small_ind,int window, uint32_t timestamp, struct timeval tv, int  nbre_tv,int *fail)
{
	int i;
	//Permet de bouger la sliding window
	for(i=0; *small_seq != seq%256; i++){
		*small_ind =  (*small_ind+1)%window; //seq est le prochain element attendu
		*small_seq = (*small_seq+1)%256;
		if(*small_seq == seq){//adapte le rtt et le temps maximal d'attente car le receiver a reçut seql
			struct timeval tv1;
			gettimeofday(&tv1, NULL);
			adapt_rtt(tv1, timestamp, &tv, nbre_tv);
		}

	}
}

void adapt_rtt(struct timeval tv1, uint32_t timestamp, struct timeval *tv, int nbre_tv){
	//Calcul du rtt actuel
	double rtt = (100000 * (tv->tv_usec) + tv->tv_sec)/2.0;

	//Calcul de la différence
	int dif = 100000 * tv1.tv_usec+ tv1.tv_sec - timestamp;
	if(nbre_tv == 10)
		rtt = rtt - rtt / 10 + dif;
	else{
		nbre_tv++;
		rtt = (rtt * (nbre_tv-1) + dif) / nbre_tv;
	}
	rtt = rtt * 2;
	tv->tv_sec = rtt / 100000;
	tv->tv_usec = (int) rtt % 100000;



}


void send_data(char *hostname, int port, char* file){
	struct sockaddr_in6 real_addr;
	memset(&real_addr, 0, sizeof(real_addr));
	const char* ret_real = real_address(hostname, &real_addr);
	if (ret_real != NULL){
		//	fprintf(stderr, "Address '%s' is not recognized.\n", hostname);
		return;
	}
	//Création du socket(qui est un file directory) à partir de l'adresse IPV6 récupérée avec la méthode real_adress
	int sfd = create_socket(NULL, 0,&real_addr, port);
	int fd;
	//Ouvre le fichier si l'utilisateur en a fournit un en argument.
	if(file != NULL)
		fd = open((const char *)file, O_RDONLY);
	else
		fd = STDIN_FILENO;

	//Création d'une structure packet qui sera utilisée pour envoyer les informations
	pkt_t* pkt_send = pkt_new();
	if(pkt_send == NULL){
		fprintf(stderr, "pkt_send : An occur failed while creating a data packet.\n");
		pkt_del(pkt_send);
		return;
	}
	//Initialisation la window du packet à 1, la window va être s'adaptée par la suite avec la window du reciever
	pkt_set_window(pkt_send,1);
	//Création d'une structure packet qui contiendra les ACK et NACK
	pkt_t* pkt_ack = pkt_new();
	if(pkt_ack == NULL){
		fprintf(stderr, "pkt_ack : An occur failed while creating a data packet.\n");
		pkt_del(pkt_ack);
		pkt_del(pkt_send);
		return;
	}
	pkt_copy(pkt_send, pkt_ack);
	int endFile = 0; //on regarde si fin du fichier ou non
	int max_length = 0; //Taille maximal du file directory, utilisé pour l'appel du select
	struct timeval tv; //Valeur du retransmission timer utilisé par défault
	tv.tv_sec = 0;
	tv.tv_usec = 100000;
	int small_seq = 0; // valeur du segment le plus petit dans le buffer
	int small_ind = 0; //Indice du plus petit paquet dans le buffer
	int seq_ind = 0; //Indice du plus grand segment dans le buffer
	int buffer_empty = 0; //indique si le buffer est vide (0, 1 sinon)
	int seq_exp = 0; //prochain numero de sequence a envoyer

	int window = 1; //Valeur initiale de la window
	int nbre_tv = 0;

	char packet_encoded[528]; //Utilisé pour stocker le packet encodé
	char read_tmp[512]; //utilisé pour lire le payload

	pkt_t *buffer[MAX_WINDOW_SIZE];


	fd_set read_set;

	int ack_received = 0;
	int sent = 0;
	int fail =0;
	while(buffer_empty==1 || endFile == 0)
	{
		if(endFile == 1)
			break;
		FD_ZERO(&read_set);
		FD_SET(sfd, &read_set);

		//Si le buffer est rempli, on ne doit pas lire de nouvelles données
		int isNotFull = seq_exp-small_seq<window;
		if(endFile == 0 && (buffer_empty == 0 || isNotFull )) {
			FD_SET(fd, &read_set);//prepare le premier flux (fichier) inutile si on est deja arrives a la fin ou si le buffer est rempli
		}


		//calcul de la taille max entre les deux file directory
		max_length = (fd > sfd) ? fd+1 : sfd+1;
		//On considère que la variable peut être modifiée après l'appel de la fonction,
		//on utilise donc une autre structure.
		struct timeval newtv = tv;

		sent  = 0;
		ack_received = 0;
		//Permet de gerer plusieurs entrées et sorties à la fois
		select(max_length, &read_set,NULL, NULL, &newtv);

		if(FD_ISSET(fd, &read_set)) {

			if(seq_ind%window !=0)
			{
				//printf("INDICE AVANT: %d\nTAILLE : %d\n",(seq_ind-1)%window,buffer_len[(seq_ind-1)%window]);
			}
			//On lit dans le fichier, et on stocke les données dans le buffer read
			//La taille maximul correspond à la taille du payload
			int length = read(fd,(void *)read_tmp, MAX_PAYLOAD_SIZE);

			//SI TAILLE 0 => Fin de fichier
			if(length == 0){

				endFile =1;
			}
			//Si TAILLE -1 => Erreur
			else if(length < 0){
				perror("Error reading file");
				pkt_del(pkt_send);
				pkt_del(pkt_ack);
				return;
			}
			//Si taille > 1 => ok, en peut remplir de données le packet
			else if(length > 0){

				if(buffer_empty == 0){
					buffer_empty = 1;
				}

				//On place, dans notre packet, les données lues dans le fichier
				pkt_set_payload(pkt_send,(const char*)read_tmp,(size_t) length);

				pkt_set_seqnum(pkt_send,seq_exp);


				pkt_t* pkt_buff  = pkt_new();
				pkt_copy(pkt_send,pkt_buff);
				buffer[seq_ind%window] = pkt_buff;


				size_t tmp = 528;

				//On encode le packet pour l'envoyer après
				if(pkt_encode(pkt_send,packet_encoded,&tmp)!= PKT_OK)
				{
					//Si le message de retour est différent de PKT_OK => Il y a eu un probème
					fprintf(stderr, "pkt_encode ici o : An occur failed while creating a data packet.\n");
					pkt_del(pkt_send);
					pkt_del(pkt_ack);
					return;
				}

				//Envoiela packet au reciever
				if(write(sfd,packet_encoded,tmp) < 0)
				{
					fprintf(stderr, "write : An occur failed while sending a packet.\n");
					pkt_del(pkt_send);
					pkt_del(pkt_ack);
					return;
				}
				else
				{
					printf("[[[ SEGMENT NUM %d SENT]]]\n",pkt_get_seqnum(pkt_send));
				}

				//printf("INDICE : %d\nTAILLE : %d\n,",seq_ind%window,buffer_len[seq_ind%window]);


				seq_exp = (seq_exp + 1)%256;
				seq_ind ++;
				sent = 1;
			}

		}
		//Cas ou on a reçu un ACK/NACK
		if( FD_ISSET(sfd, &read_set)){
			//Lecture du packet encodé recu et le place dans la variable packet_encoded
			int length = read(sfd, (void *)packet_encoded, 528);
			//Decodage du  packet reçu dans pkt_ack
			if(length> 0 && pkt_decode((const char *)packet_encoded,(size_t )length,pkt_ack) != PKT_OK){
				fprintf(stdout,"[[[ ERROR RECIEVING ACK NUM %d ]]]\n",pkt_get_seqnum(pkt_ack));
			}
			else
			{
				int i = pkt_get_type(pkt_ack);
				int seqRet = pkt_get_seqnum(pkt_ack);
				printf("seqRet : %d\n", seqRet);
				if(i == 1)
				{
					fprintf(stdout,"[[[ SEGMENT NUM %d RECIEVED ]]]\n",seqRet);
				}
				else if(i == 2)
				{
					fprintf(stdout,"[[[ ACK NUM %d RECIEVED ]]]\n",seqRet);

				}
				else if(i == 3)
				{
					fprintf(stdout,"[[[ NACK %d RECIEVED ]]]\n",seqRet);
				}
				else{
					printf("[[[ ERROR ON SEGNUM RECIEVED ]]]\n");
				}


				int window1 = pkt_get_window(pkt_ack);

				uint8_t seq = pkt_get_seqnum(pkt_ack);

				if(window1 > window){ //le buffer etait rempli
					window = window1;
				}


				pkt_set_window(pkt_send, window); //changera pas si window invalide
				ack_received = 1;
				if(pkt_get_type(pkt_ack)==PTYPE_ACK){
					//int w = 0;
					adapt_buffer(&small_seq, seq, &small_ind,window, pkt_get_timestamp(pkt_ack), tv, nbre_tv, &fail);
					if(fail == 1)
					{
						size_t tmp = 528;
						//On encode le packet pour l'envoyer après
						if(pkt_encode(buffer[small_ind%window],packet_encoded,&tmp)!= PKT_OK)
						{
							//Si le message de retour est différent de PKT_OK => Il y a eu un probème
							fprintf(stderr, "pkt_encode ici o : An occur failed while creating a data packet.\n");
							pkt_del(pkt_send);
							pkt_del(pkt_ack);
							return;
						}

						//Envoiela packet au reciever
						if(write(sfd,packet_encoded,tmp) < 0)
						{
							fprintf(stderr, "write : An occur failed while sending a packet.\n");
							pkt_del(pkt_send);
							pkt_del(pkt_ack);
							return;
						}
						else
						{
							printf("[[[ SEGMENT NUM %d SENT]]]\n",pkt_get_seqnum(buffer[small_ind%window]));
						}
						fail =0;
					}
					if(endFile == 1 && small_ind == (seq_exp)) {
						buffer_empty = 0;
					}


				}
				else if(pkt_get_type(pkt_ack) == PTYPE_NACK) {
					if(seq < small_seq)
					{
						seq+=256;
					}
					int index = small_ind%window + seq + (seq - small_seq);
					size_t tmp = 528;
					//On encode le packet pour l'envoyer après
					if(pkt_encode(buffer[index],packet_encoded,&tmp)!= PKT_OK)
					{
						//Si le message de retour est différent de PKT_OK => Il y a eu un probème
						fprintf(stderr, "pkt_encode ici o : An occur failed while creating a data packet.\n");
						pkt_del(pkt_send);
						pkt_del(pkt_ack);
						return;
					}

					//Envoiela packet au reciever
					if(write(sfd,packet_encoded,tmp) < 0)
					{
						fprintf(stderr, "write : An occur failed while sending a packet.\n");
						pkt_del(pkt_send);
						pkt_del(pkt_ack);
						return;
					}
					else
					{
						printf("[[[ SEGMENT NUM %d SENT]]]\n",pkt_get_seqnum(buffer[small_ind%window]));
					}

					if(write(sfd,buffer[index],tmp) < 0)
					{
						fprintf(stderr, "write : An occur failed while sending a packet.\n");
						pkt_del(pkt_send);
						pkt_del(pkt_ack);
						return;
					}

				}


			}
		}

		if(sent !=1 && ack_received == 0 && file != NULL)
		{


			size_t tmp = 528;

			//On encode le packet pour l'envoyer après
			if(pkt_encode(buffer[small_ind%window],packet_encoded,&tmp)!= PKT_OK)
			{
				//Si le message de retour est différent de PKT_OK => Il y a eu un probème
				fprintf(stderr, "pkt_encode ici : An occur failed while creating a data packet.\n");
				pkt_del(pkt_send);
				pkt_del(pkt_ack);
				return;
			}

			if(write(sfd, packet_encoded,tmp)  < 0)
			{
				fprintf(stderr, "write : An occur failed while sending a packet.\n");
				pkt_del(pkt_send);
				pkt_del(pkt_ack);
				return;
			}
			printf("[[[ SEGMENT NUM %d SENT BY RETRANSMISSION TIMER]]]\n",pkt_get_seqnum(buffer[small_ind%window]));
		}



		
	} //FIN DE LA BOUCLE
	
	//On envoie un packet de taille 0 pour indiquer la fin du transfert de fichier
	int endAck = 0;
	pkt_set_length(pkt_send,0);
	pkt_set_seqnum(pkt_send,seq_exp);
	size_t tmp = 528;
	if(pkt_encode(pkt_send,packet_encoded,&tmp)!= PKT_OK)
	{
		//Si le message de retour est différent de PKT_OK => Il y a eu un probème
		fprintf(stderr, "pkt_encode ici o : An occur failed while creating a data packet.\n");
		pkt_del(pkt_send);
		pkt_del(pkt_ack);
		return;
	}
	while(endAck == 0)
	{
		FD_ZERO(&read_set);
		FD_SET(sfd, &read_set);
		struct timeval newtv = tv;
		select(sfd+1, &read_set,NULL, NULL, &newtv);
		printf("[[[SEG END SEND]]]\n");	
		//On envoie le message tant qu'on a pas eu l'ACK
		if(write(sfd,packet_encoded,tmp) < 0)
		{
			fprintf(stderr, "write : An occur failed while sending a packet.\n");
			pkt_del(pkt_send);
			pkt_del(pkt_ack);
			return;
		}
		//Des qu'on a recu l'ack, on peut fermer le programme
		if(FD_ISSET(sfd, &read_set)) {
			int length = read(sfd, (void *)packet_encoded, 528);
			//Decodage du  packet reçu dans pkt_ack
			if(pkt_decode((const char *)packet_encoded,(size_t )length,pkt_ack) == PKT_OK){
				printf("[[[ACK END RECEIVED]]]");	
				
					endAck = 1;
				
			}
		}
	}
	
	close(sfd);
	close(fd);
	pkt_del(pkt_ack);
	pkt_del(pkt_send);
}
