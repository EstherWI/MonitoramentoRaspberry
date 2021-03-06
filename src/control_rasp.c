#include <stdio.h>
#include <string.h>
#include <wiringPi.h>
#include <time.h>
#include <unistd.h>
#include <lcd.h>
#include "ads/ads1115_rpi.h"
#include "ads/ads1115_rpi.c"
#include "ads/ads1115.c"
#include "dht11/DHT11library.h"
#include "dht11/DHT11library.c"
#include <mosquitto.h>

// list wiringPi-RPi pins $ gpio readall

// gcc -o file file.c -lwiringPi -lwiringPiDev -lmosquitto

//Configurações do Broker
#define MQTT_ADDRESS   "10.0.0.101"
#define CLIENTID       "clientID"  
#define USERNAME "aluno"
#define PASSWORD "aluno*123"

/*Topicos de publish e subscribe*/
#define MQTT_PUBLISH_TEMP    "medida/temperatura"
#define MQTT_PUBLISH_UMID    "medida/umidade"
#define MQTT_PUBLISH_PRESSAO    "medida/pressaoAtm"
#define MQTT_PUBLISH_LUMI    "medida/luminosidade"
#define MQTT_PUBLISH_TEMPO    "config/tempo"
#define MQTT_PUBLISH_HISTORICO   "historico"

#define DHT11PIN 5
#define DEBOUNCE_DELAY  1

unsigned long int debounce_last_timestamp = 0;
float temperatura, umidade, luminosidade, pressao;
float temperaturaH[10], umidadeH[10], luminosidadeH[10], pressaoH[10];
char dataH[10][20], horaH[10][20];
char historico[500];
int historicoIndex = 0;
int historicoQtd = 0;

int lcd;
int menuLocalizacao = 0;
int menuPosicao = 0;
int changeInterface = 1;
int menuHistorico = 0;
int configTempo = 5;
int chaveTempo = 0;
// variaveis para armazenar o nivel logico das chaves que configuram o tempo
//4 17 27 22 - esses são numeros na placa é necessario trocar para a numeração do wiringPi
int chaveT1 = 7;
int chaveT2 = 0;
int chaveT3 = 2;
int chaveT4 = 3;

char menu2nivel = '*';
char menu3nivel = '-';
char menuOpcoes[3][32] = {
    "1: Acompanhar em tempo real",
    "2: Historico",
    "3: Configurar   tempo"
};

int rc;
struct mosquitto * mosq;

void resetLcd(int lcd);
void printMedidas();
void menu();
void proximo();
void voltar();
void confirmar();
void updateMedidas();
void updateHistorico();
float mapValue(float value, float max, float offset);
void remoteUpdateMQTT();
void updateConfigTempo();
void updateChaveTempo();
int getChaveTempo();
int debounce();
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message);
void getHistorico();

PI_THREAD (medidasThread)
{
    while (1)
    {
        updateMedidas();
        remoteUpdateMQTT();
        usleep(configTempo * 1000000);
    }
}

//Função para atualizar as medições no Broker MQTT
void remoteUpdateMQTT(){

    char temp[10], umid[10], luz[10], pressaoAtm[10];
    //formatando medições para envio no Broker MQTT
    sprintf(temp, "%.2f", temperatura);
    sprintf(umid, "%.2f", umidade);
    sprintf(luz, "%.2f", luminosidade);
    sprintf(pressaoAtm, "%.2f", pressao);

    mosquitto_publish(mosq, NULL, MQTT_PUBLISH_TEMP , strlen(temp), temp, 0, false);
    mosquitto_publish(mosq, NULL, MQTT_PUBLISH_UMID , strlen(umid), umid, 0, false);
    mosquitto_publish(mosq, NULL, MQTT_PUBLISH_PRESSAO , strlen(pressaoAtm), pressaoAtm, 0, false);
    mosquitto_publish(mosq, NULL, MQTT_PUBLISH_LUMI , strlen(luz), luz, 0, false);

}


int main(){
    mosquitto_lib_init();

    //Cria uma nova instância do cliente
    mosq = mosquitto_new(CLIENTID, true, NULL);

    //Configura user e senha para o cliente
    mosquitto_username_pw_set(mosq,USERNAME, PASSWORD);
    
    //Define o callback da mensagem (é chamada quando uma mensagem é recebida do broker)
	mosquitto_message_callback_set(mosq, on_message);

    //Conecta no Broker MQTT
    rc = mosquitto_connect(mosq, MQTT_ADDRESS, 1883, 60);
    if(rc != 0){
        printf("Client could not connect to broker! Error Code: %d\n", rc);
        mosquitto_destroy(mosq);
        return -1;
    }

    //Inscreve no tópico tempo (configuração do tempo de medição)
    mosquitto_subscribe(mosq, NULL, MQTT_PUBLISH_TEMPO, 0);
    mosquitto_loop_start(mosq);


    printf("We are now connected to the broker!\n");
    wiringPiSetup();
    lcd = lcdInit(2,16,4,6,31,26,27,28,29,0,0,0,0);

    // configuração do ADS1115 
    configADS1115();
    // inicialização do sensor DHT11
    InitDHT(DHT11PIN);

    wiringPiISR (21, INT_EDGE_FALLING, &voltar);//botão voltar
    wiringPiISR (24, INT_EDGE_FALLING, &proximo);//botão proximo
    wiringPiISR (25, INT_EDGE_FALLING, &confirmar);//botão confirmar
    wiringPiISR (chaveT1, INT_EDGE_BOTH, &updateChaveTempo);
    wiringPiISR (chaveT2, INT_EDGE_BOTH, &updateChaveTempo);
    wiringPiISR (chaveT3, INT_EDGE_BOTH, &updateChaveTempo);
    wiringPiISR (chaveT4, INT_EDGE_BOTH, &updateChaveTempo);

    int x = piThreadCreate(medidasThread);
    if (x !=0 ){
        printf("Erro ao iniciar a thread.");
    }

    menu();//a função menu exibe a interface homem-maquina nas raps e está em loop infinito

    return 0;
}

/**
 * Função que escreve todas as informações do historico em uma unica string para envio no Broker MQTT
 * A string é formatada num padrão especifico que é reconhecido no aplicativo.
 */
void getHistorico(){
    char historicoi[50];
    sprintf(historico,"");
    unsigned int index;
    for(index=0; index<historicoQtd; index++){
        sprintf(historicoi, "%.2f|%.2f|%.2f|%.2f|%s|%s;", 
            temperaturaH[index], luminosidadeH[index], umidadeH[index], pressaoH[index], dataH[index], horaH[index]);
        strcat(historico, historicoi);
        printf("%d/%d: %s\n", index, historicoQtd, historicoi);
    }
}

/**
 * Função auxiliar para limpa a tela do LCD e resetar o ponteiro para a posição inicial
 */
void resetLcd(int lcd){
    lcdClear(lcd);
    lcdPosition(lcd, 0, 0);
}

/**
 * Exibe as medições atuais no sensor LCD
 */
void printMedidas(){

    resetLcd(lcd);
    lcdPrintf(lcd,"%.1f C | %.1f I", temperatura, luminosidade);
    lcdPosition(lcd, 0, 1);
    lcdPrintf(lcd,"%.1f U | %.1f Pa", umidade, pressao);
}


/**
 * Exibe as informações de uma medição do historico no lcd, de acordo o historicoIndex
 * As medições ou a data e hora podem ser exibidas, a depender da opção do menu, 
 */
void printHistorico(){
    if(menuHistorico){ // 1 para exibir medidas
        resetLcd(lcd);
        lcdPrintf(lcd,"%.1f C | %.1f I %d", temperaturaH[historicoIndex], luminosidadeH[historicoIndex], historicoIndex );
        lcdPosition(lcd, 0, 1);
        lcdPrintf(lcd,"%.1f U | %.1f Pa", umidadeH[historicoIndex], pressaoH[historicoIndex] );
    }else{ // 0 para exibir data e hora
        resetLcd(lcd);
        lcdPrintf(lcd,"%s     %d", dataH[historicoIndex], historicoIndex);
        lcdPosition(lcd, 0, 1);
        lcdPrintf(lcd, "%s", horaH[historicoIndex] );
    }
}

/**
 * Função que implementa a interface homem-maquina, as variaveis menuLocalizacao e menuPosicao controlam o que é exibido na rasp.
 * Essas variaveis são alteradas conforme os botões da rasp são pressionados.
 * */
void menu(){
    char mensagemTempo1[16] = "";
    char mensagemTempo2[16] = "";

    while(1){
        // A interface no lcd só é atualizada quando ocorre algum evento/ação, isso retira um efeito do LCD de estar "piscando".
        // O "piscando" era causado porque a tela do LCD era limpa e a mesma informação era colocada novamente.
        if(changeInterface){
            if(menuLocalizacao == 0){//menu principal
                resetLcd(lcd);
                lcdPuts(lcd, menuOpcoes[menuPosicao]);
            }
            else if (menuLocalizacao == 1){//medidas atuais
                printMedidas();
            }
            else if (menuLocalizacao == 2){//historico
                printHistorico();
            }
            else if (menuLocalizacao == 3){//configuração do tempo
                updateChaveTempo();
                sprintf(mensagemTempo1, "Atual: %d s", configTempo);
                sprintf(mensagemTempo2, "Chave: %d s", chaveTempo);
                resetLcd(lcd);
                lcdPuts(lcd, mensagemTempo1);
                lcdPosition(lcd, 0, 1);
                lcdPuts(lcd, mensagemTempo2);
            }
            changeInterface = 0;// para que na proxima interação o lcd não seja atualizado sem necessidade
        }
    }
    
}

/**
 * Função que é associado ao botão "proximo" e altera o menu de acordo a essa função no contexto atual
 * O contexto se refere a qual lugar do menu o usuario está visualizando.
 * */
void proximo(){
    if( !debounce() ) {
        return;
    }

    if (menuLocalizacao == 2){//historico
        historicoIndex++;
        if(historicoIndex >= historicoQtd ){
            historicoIndex = 0;
        }
    }
    else if (menuLocalizacao == 0) {//menu principal
        menuPosicao >= 2 ? menuPosicao = 0 : menuPosicao++;
    }
    changeInterface = 1;
}


/**
 * Função que é associado ao botão "proximo" e altera o menu de acordo a essa função no contexto atual
 * O contexto se refere a qual lugar do menu o usuario está visualizando.
 * */
void confirmar(){
    if( !debounce() ) {
        return;
    }

    if (menuLocalizacao == 0) {
        menuLocalizacao = menuPosicao+1;
        menuHistorico=0; // exibe data e hora por padrão
    }
    else if (menuLocalizacao == 1) { //quando estiver exibindo as medidas atuais o botão confirmar força a atualização das medidas
        updateMedidas();
    }
    else if (menuLocalizacao == 2) { // altera exibição no historico de data e hora para medidas ou vice-versa.
        menuHistorico = 1 - menuHistorico;
    }
    else if (menuLocalizacao == 3) { //confirma a atualização do tempo e publica a mesma no topico MQTT
        updateConfigTempo();
    }
    changeInterface = 1;
}

void voltar(){
    if( !debounce() ) {
        return;
    }
    if(menuLocalizacao == 0){//voltar uma opção no menu principal
        menuPosicao <= 0 ? menuPosicao = 2 : menuPosicao--;
    }
    else {
        menuLocalizacao = 0;
    }
    changeInterface = 1;
}

void updateMedidas(){
    luminosidade = mapValue(getLuminosity(), 10, 0);//mapeando a luminosidade na faixa de 0-10
    pressao = mapValue(getPressure(), 11, 3);//mapeando a pressão na faixa de 3-11
    read_dht11_dat();
    temperatura = getTemp();
    umidade = getHumidity();
    updateHistorico();
    changeInterface =1;
}

void updateHistorico(){
    time_t tempo;
    time(&tempo);
    struct tm *tempo0 = localtime(&tempo);

    for(int i=historicoQtd-1; i>0; i--){//movendo as medições anteriores para trás, as novas medições ficam na primeira posição
        temperaturaH[i] = temperaturaH[i-1];
        umidadeH[i] = umidadeH[i-1];
        pressaoH[i] = pressaoH[i-1];
        luminosidadeH[i] = luminosidadeH[i-1];
        sprintf(dataH[i], "%s", dataH[i-1]);
        sprintf(horaH[i], "%s", horaH[i-1]);
    }
    temperaturaH[0] = temperatura; 
    umidadeH[0] = umidade; 
    pressaoH[0] = pressao; 
    luminosidadeH[0] = luminosidade; 
    sprintf(dataH[0], "%02d/%02d/%02d", tempo0->tm_mday, tempo0->tm_mon+1, 1900+tempo0->tm_year);
    sprintf(horaH[0], "%02d:%02d:%02d", tempo0->tm_hour, tempo0->tm_min, tempo0->tm_sec);
    getHistorico();
    printf("historico: %s\n", historico);
    //Publica o histórico completo no broker
    mosquitto_publish(mosq, NULL, MQTT_PUBLISH_HISTORICO , strlen(historico), historico, 0, false);
    
    if(historicoQtd < 10) 
        historicoQtd++;
}

void updateConfigTempo(){
    char charTempo[4];

    configTempo = chaveTempo;
    changeInterface = 1;

    sprintf(charTempo, "%d", configTempo);
    //Publica a configuração do tempo no broker MQTT
    mosquitto_publish(mosq, NULL, MQTT_PUBLISH_TEMPO , strlen(charTempo), charTempo, 0, false);
}

void updateChaveTempo(){
    // if( !debounce() ) {
    //     return;
    // }
    chaveTempo = getChaveTempo();
    changeInterface = 1;
}

/**
 * Função que retorna o valor do tempo de acordo as chaves, sendo que a chave de maior valor ativa é priorizada.
 **/
int getChaveTempo(){
    if(!digitalRead(chaveT4)){
        return 100;
    }
    if(!digitalRead(chaveT3)){
        return 80;
    }
    if(!digitalRead(chaveT2)){
        return 60;
    }
    if(!digitalRead(chaveT1)){
        return 40;
    }
    return 20;//valor default caso nenhuma chave esteja ativa
}

//Callback - Sempre que uma mensagem é recebida do broker
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
	printf("Nova mensagem\n %s: %s\n", msg->topic, (char *) msg->payload);
    configTempo = atoi(msg->payload);
    changeInterface = 1;
}

/**
 * Função para mapear valores dentro de uma faixa especifica.
 * Exemplo para a faixa de 10-100, o max deve ser 100 e o offset é definido como 10.
 * */
float mapValue(float value, float max, float offset){
    return (float) offset + ((value *( max-offset)) / 3.3);
}

int debounce(){
    unsigned long int timestamp = time(NULL);
    piLock(0);

    if(timestamp - debounce_last_timestamp > DEBOUNCE_DELAY){
        debounce_last_timestamp = timestamp;
        piUnlock(0);
        return 1;
    }
    piUnlock(0);
    return 0;
}