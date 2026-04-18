# Sistema Tático Multimissão AT-04 — Dominação + Ignitor RFID

Este projeto implementa um sistema de controle de "mísseis" (simulados) baseado em ESP32, combinando:
- **RFID MFRC522** para autenticação de usuários
- **Botões físicos** para acionamento e captura
- **LCD I2C** para exibição de status e placar
- **Lógica de jogo** com cronometragem e controle de posse
- **Sistema de segurança** com relé para ignição real

## 📋 Pré-requisitos

- ESP-IDF v5.x instalado
- Ferramentas de compilação e upload (idf.py, esptool.py)

## ⚙️ Configuração do Projeto

1. **Clone o repositório:**
   ```bash
   git clone <url-do-repositorio>
   cd esba-missil
   ```

2. **Configure o menuconfig:**
   ```bash
   idf.py menuconfig
   ```
   - Verifique as configurações de I2C, SPI e GPIOs em:
     - `Component config` → `I2C`
     - `Component config` → `SPI`
     - `Component config` → `Main` (para pinos customizados)

3. **Edite as constantes no código:**
   - `main/rfid_rc522.h`: ajuste `RC522_PIN_RST` se necessário
   - `main/lcd_i2c.h`: ajuste `LCD_I2C_ADDR`, `LCD_I2C_SDA_PIN`, `LCD_I2C_SCL_PIN`
   - `main/main.c`: ajuste `AUTHORIZED_UID` com o UID da sua TAG

## 🚀 Compilação e Upload

```bash
# Compilar
idf.py build

# Gravar no ESP32
idf.py -p COM3 -b 460800 flash

# Monitorar serial
idf.py -p COM3 monitor
```

## 🔌 Conexões Recomendadas

### RFID MFRC522
- **SDA** → GPIO 21
- **SCK** → GPIO 18
- **MOSI** → GPIO 23
- **MISO** → GPIO 19
- **RST** → GPIO 4
- **3.3V** → 3.3V
- **GND** → GND

### LCD I2C (PCF8574 + HD44780)
- **SDA** → GPIO 22
- **SCL** → GPIO 21
- **VCC** → 5V
- **GND** → GND

### Botões e Relé
- **Botão Captura** → GPIO 13
- **Botão Ignitor** → GPIO 12
- **Relé (trigger)** → GPIO 4 (ou outro GPIO disponível)

> **Nota:** Se usar GPIO 22 para SCL do LCD, mova o pino RST do MFRC522 para GPIO 4 (ou outro disponível) para evitar conflito.

## 🎯 Funcionamento

### 🔐 Segurança (Task_Security - Core 0)
1. Lê continuamente o RFID MFRC522
2. Quando detecta uma TAG autorizada:
   - Ativa o relé por 3 segundos (ignição)
   - Incrementa o placar do time "Dominação"
   - Exibe "MISSEL DISPARADO" no LCD
3. Mantém o relé desativado caso contrário

### 🎮 Jogo (Task_GameLogic - Core 1)
1. Monitora botão de captura (hold 5s)
2. Mantém cronômetro de posse
3. Atualiza placar "Dominação" e "Ignitor" conforme ações
4. Controla estado de captura e ignição

### 📱 Interface (Task_UI - Core 1)
1. Atualiza LCD a cada 500ms
2. Exibe:
   - Status do sistema (READY, CAPTURING, IGNITED)
   - Placar "Dominação" e "Ignitor"
   - Tempo de captura (MM:SS)
   - Mensagens de status

## 📊 Estados do Sistema

- **READY**: Aguardando captura ou leitura de TAG
- **CAPTURING**: Usuário pressionando botão por 5 segundos
- **IGNITED**: Míssil disparado (relé ativado por 3s)

## 🔧 Customização

### Mudar TAG Autorizada
No arquivo `main/main.c`, localize:
```c
static const uint8_t AUTHORIZED_UID[] = {0x12, 0x34, 0x56, 0x78};
```
Substitua pelo UID da sua TAG (ex: `0xDE, 0xAD, 0xBE, 0xEF`).

### Ajustar Pinos
No arquivo `main/lcd_i2c.h`:
```c
#define LCD_I2C_ADDR            0x27
#define LCD_I2C_SDA_PIN         22
#define LCD_I2C_SCL_PIN         21
```
No arquivo `main/rfid_rc522.h`:
```c
#define RC522_PIN_RST           4
```

## ⚠️ Considerações de Segurança

- O relé é acionado diretamente pelo ESP32 (máx 20mA)
- Para cargas maiores, use um transistor MOSFET ou relé driver
- O sistema de ignição deve ser isolado eletricamente da lógica
- Teste com cargas seguras antes de conectar ao sistema real

## 📝 Licença

MIT

## 📌 Mapa Completo de Pinos (GPIO)

| GPIO | Função | Periférico | Direção | Definido em |
|:----:|--------|------------|---------|-------------|
| 4 | RST (Reset) | RFID MFRC522 | Saída | `rfid_rc522.h` → `RC522_PIN_RST` |
| 5 | CS / SS (Chip Select) | RFID MFRC522 | Saída | `rfid_rc522.h` → `RC522_PIN_CS` |
| 13 | Botão Equipe Amarela | Entrada digital | Entrada (Pull-up) | `game_logic.h` → `BTN_YELLOW_GPIO` |
| 14 | Botão Equipe Azul | Entrada digital | Entrada (Pull-up) | `game_logic.h` → `BTN_BLUE_GPIO` |
| 18 | SCK (SPI Clock) | RFID MFRC522 | Saída | `rfid_rc522.h` → `RC522_PIN_SCK` |
| 19 | MISO (SPI Master In) | RFID MFRC522 | Entrada | `rfid_rc522.h` → `RC522_PIN_MISO` |
| 21 | SDA (I2C Data) | LCD PCF8574 | Bidirecional | `lcd_i2c.h` → `LCD_I2C_SDA_PIN` |
| 22 | SCL (I2C Clock) | LCD PCF8574 | Saída | `lcd_i2c.h` → `LCD_I2C_SCL_PIN` |
| 23 | MOSI (SPI Master Out) | RFID MFRC522 | Saída | `rfid_rc522.h` → `RC522_PIN_MOSI` |
| 26 | Buzzer (feedback sonoro) | Buzzer passivo | Saída | `game_logic.h` → `BUZZER_GPIO` |
| 27 | Relé de Ignição | Módulo relé | Saída | `game_logic.h` → `RELAY_GPIO` |

> **Total:** 11 GPIOs utilizados de 34 disponíveis no ESP32.