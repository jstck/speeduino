#ifndef CAN_H
#define CAN_H

#define CAN_CS_PIN 10
#define CAN_SPEED CAN_1000KBPS

void initCAN();
void sendCAN();

#endif //CAN_H
