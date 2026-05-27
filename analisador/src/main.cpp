#include <nRF24L01.h>
#include <RF24.h>
#include <Arduino.h>

RF24 radio(4, 5); // CE=4, CSN=5

long weight;

void setup()
{
  Serial.begin(115200);
  Serial.println("Iniciando o transmissor...");
  radio.begin();
  radio.setChannel(5);
  radio.setDataRate(RF24_1MBPS);
  radio.setPALevel(RF24_PA_HIGH);
  radio.openWritingPipe(0x1234567890LL);
}

void loop()
{
  weight = 13381;
  radio.write(&weight, sizeof(weight));
  Serial.print("Peso enviado: ");
  Serial.println(weight);
  delay(1000);
}