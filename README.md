# fft-application

Firmware e simulação Wokwi para leitura de sinal tipo geofone (diferencial), amplificação de instrumentação e conversão ADS1256, com registro em cartão microSD (CSV).

## Objetivo

- **Hardware / firmware**: Arduino Uno lê o ADC via SPI, grava amostras em `data.csv` no SD e pode depurar pela serial.
- **Simulação**: O `diagram.json` monta geofone → amplificador (`chip-instamp`) → ADC (`chip-ads1256`) → cartão SD, espelhando o encadeamento do projeto.

## Estrutura do repositório

| Caminho | Função |
|--------|--------|
| `src/main.cpp` | Firmware principal (SPI ADS1256, SD, serial). |
| `platformio.ini` | Ambiente PlatformIO (board `uno`, libs SD etc.). |
| `diagram.json` | Esquema Wokwi (partes e ligações). |
| `wokwi.toml` | Aponta firmware `.hex`/`.elf` e registra os chips customizados. |
| `chips/` | Código-fonte (`.chip.c`), metadados (`.chip.json`) e binários (`.chip.wasm`) dos chips Wokwi. |
| `wokwi-api.h` | Cabeçalho da API Wokwi usado na compilação WASM dos chips (mantido na raiz do projeto). |
| `pyproject.toml`, `uv.lock` | Dependências Python do projeto (PlatformIO) e lockfile gerenciados pelo **uv**. |

## Como rodar (passo a passo)

### 1. Ferramentas base

1. **Instalar uma IDE**: [Cursor](https://cursor.com) ou [Visual Studio Code](https://code.visualstudio.com). Serve para editar o firmware, o `diagram.json` e instalar a extensão **Wokwi Simulator**.

2. **Instalar extensões na IDE**:
   - **Python**
   - **Python Debugger** (ou depuração Python integrada à extensão Python)
   - **Wokwi Simulator** (simulação do circuito a partir do `diagram.json`)

3. **Obter o código** — Clonar o repositório com Git e abrir a **pasta raiz** do projeto na IDE (onde ficam `pyproject.toml`, `diagram.json` e `wokwi.toml`). Os comandos `uv`, `pio` e `wokwi-cli` abaixo assumem que o terminal está nessa pasta. Quem ainda não tiver o Git pode instalar a partir de [git-scm.com/downloads](https://git-scm.com/downloads).

```bash
git clone https://github.com/Henrique-BL/fft-application.git
cd fft-application
```

   Quem não usar Git pode baixar o ZIP em **Code → Download ZIP** em [github.com/Henrique-BL/fft-application](https://github.com/Henrique-BL/fft-application), extrair e usar **File → Open Folder** na IDE para abrir a pasta extraída.

4. **Instalar Python 3.11 ou superior** — O projeto declara `requires-python >= 3.11` em `pyproject.toml`. Esse runtime é usado pelo **uv**/PlatformIO e pelas extensões Python da IDE.

   - **Windows**: Baixar o instalador em [python.org/downloads](https://www.python.org/downloads/) (escolher 3.11+). No assistente, marcar **Add python.exe to PATH** antes de concluir. Alternativa via terminal: `winget install Python.Python.3.12` (ou outra versão ≥ 3.11 listada pelo `winget search Python.Python`).
   - **macOS** (Homebrew): `brew install python@3.12` (ou `python@3.11`), depois seguir a dica do `brew` para expor o binário no `PATH`, se necessário.
   - **Linux**: Usar o Python do repositório da distro (≥ 3.11) ou o [guia de instalação](https://wiki.python.org/moin/BeginnersGuide/Download) oficial; em Ubuntu antigo, pode ser preciso o PPA [deadsnakes](https://launchpad.net/~deadsnakes/+archive/ubuntu/ppa).

   **Conferir**: `python --version` ou `python3 --version` deve mostrar 3.11 ou maior. No Windows, o *launcher* `py -3.11 --version` (ou `py -3 --version`) também pode ser usado.

5. **Instalar o uv e sincronizar dependências** (na raiz do clone): cria o ambiente virtual (`.venv`) e instala o PlatformIO conforme `pyproject.toml` / `uv.lock`.

```bash
python -m pip install uv
uv sync
```

   Em sistemas em que só existir `python3`, usar `python3 -m pip install uv`.

### 2. Licença Wokwi

- Obter a **chave de licença** pelo fluxo da extensão (costuma abrir um pop-up) ou acesse:  
  https://wokwi.com/license?v=3.5.0&r=LicenseMissing&s=cursor  
- Configure a chave onde a extensão Wokwi indicar (configurações da IDE / Wokwi).

### 3. Wokwi CLI e compilação dos chips

1. **Instalar o Wokwi CLI** (ex.: via npm, conforme a [documentação oficial](https://docs.wokwi.com/wokwi-cli)):  
   `npm install -g @wokwi/cli`
2. **Compilar os chips customizados** (gera/atualiza os `.wasm` referenciados no `wokwi.toml`):

```bash
wokwi-cli chip compile chips/geophone.chip.c
wokwi-cli chip compile chips/instamp.chip.c
wokwi-cli chip compile chips/ads1256.chip.c
```

### 4. Firmware no Arduino Uno

1. **Conectar o Arduino Uno** ao computador (USB).
2. Na **pasta raiz do clone** (a mesma aberta na IDE), compilar (e, se quiser gravar, usar upload). Usa-se `uv run` para acionar o PlatformIO instalado pelo `uv sync`:

```bash
uv run pio run
# opcional: gravar na placa
uv run pio run -t upload
```

3. Monitor serial (opcional): `uv run pio device monitor` (baudrate em `platformio.ini`, ex.: 115200).

### 5. Simular no Wokwi

- Abrir o projeto na IDE com a extensão Wokwi e iniciar a simulação a partir do `diagram.json` (após `uv run pio run` gerar `.pio/build/uno/firmware.hex`, alinhado ao `wokwi.toml`).

---
