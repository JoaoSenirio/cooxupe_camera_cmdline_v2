# Capture Stability Notes

## Context

Este documento registra os principais aprendizados da comparação entre:

- `cooxupe_camera_cmdline_v2`
- `cooxupe_camera_interface`

Objetivo: evitar a volta do travamento observado no meio da captura `LIGHT` ou na transição `LIGHT -> DARK`.

Premissa operacional importante:

- O problema desapareceu quando o projeto foi refeito do zero, mantendo a mesma biblioteca SpecSensor, sem troca de versão e sem reinstalação.
- Isso indica que o defeito tinha forte relação com empacotamento/build/runtime do projeto antigo, e não com a versão da SDK.

## Leitura correta deste documento

Há dois tipos de afirmação aqui:

- `Evidência no código`: diferença objetiva encontrada nos repositórios.
- `Inferência técnica`: conclusão plausível a partir do código e do histórico da investigação, mas não comprovada por um artefato explícito versionado no repositório.

Ponto importante:

- Eu não encontrei no repositório antigo uma referência explícita ao nome da DLL problemática de threads.
- A hipótese da DLL de threads continua consistente com as diferenças de build/runtime, mas aqui ela deve ser tratada como `inferência forte`, não como prova documental completa.

## Resumo executivo

As diferenças que mais provavelmente contribuíram para o fim do travamento são estas, em ordem de relevância:

1. O projeto novo tornou a resolução das DLLs da SDK mais determinística.
2. O fluxo `LIGHT -> DARK` ficou mais defensivo e explícito.
3. O projeto novo separou melhor captura, pipe e gravação, mantendo o uso da SDK concentrado.
4. A configuração da câmera ficou mais simples e com menos fallback/auto-resolução.
5. O projeto novo passou a ter testes que congelam o contrato da transição entre fases.

## Diferenças mais importantes

### 1. Resolução de DLL da SDK ficou explícita no projeto novo

`Evidência no código`

- O projeto novo chama `SetDllDirectoryW(...)` antes de `SI_Load(...)` em `src/specsensor_api_sdk.cpp:84-106`.
- O projeto antigo faz `SI_Load(...)` diretamente, sem ajuste explícito do diretório de DLL, em `../cooxupe_camera_interface/cooxupe_camera_interface/specsensor_device.cpp:98-107`.

`Por que isso importa`

- Isso reduz o risco de o executável carregar DLLs erradas por ordem de busca do Windows.
- Isso é altamente compatível com a hipótese que você levantou sobre uma DLL de threads/runtime errada ou incompatível aparecendo no ambiente do projeto antigo.

### 2. O build novo é menos dependente de um `bin` genérico compartilhado

`Evidência no código`

- O projeto novo resolve `SpecSensorSdkDir`, inclui `include/bin/bin\\x64` da própria SDK e copia os binários dessa origem no post-build: `specsensor_cli.vcxproj:45-50`, `specsensor_cli.vcxproj:55-80`, `specsensor_cli.vcxproj:98-103`.
- O projeto antigo dependia de um `..\..\..\bin\$(Platform)\` genérico e copiava todo esse conteúdo para a saída: `../cooxupe_camera_interface/cooxupe_camera_interface/cooxupe_camera_interface.vcxproj:43-67`, `../cooxupe_camera_interface/cooxupe_camera_interface/cooxupe_camera_interface.vcxproj:80-88`.

`Inferência técnica`

- Se havia uma DLL errada de runtime/threads nesse `bin` compartilhado, o projeto antigo tinha muito mais chance de empacotá-la ou resolvê-la acidentalmente.
- O projeto novo reduz esse risco ao amarrar melhor a origem da SDK e o diretório de busca em runtime.

### 3. A transição `LIGHT -> DARK` ficou explicitamente saneada

`Evidência no código`

- No projeto novo, o fluxo é:

```text
CreateBuffer
Acquisition.Start
Acquisition.RingBuffer.Sync
Camera.OpenShutter
LIGHT loop
Camera.CloseShutter
Acquisition.Stop
sleep 250 ms
Acquisition.Start
DARK loop
Acquisition.Stop
DisposeBuffer
```

- Esse fluxo está em `src/capture_core.cpp:367-379`, `src/capture_core.cpp:515-639`, `src/capture_core.cpp:643-731`.
- O projeto antigo não faz `Acquisition.RingBuffer.Sync`, não tem atraso explícito entre `Stop` e novo `Start`, e recria o buffer entre as fases:
  - `LIGHT`: `../cooxupe_camera_interface/cooxupe_camera_interface/acquisition_runner.cpp:213-321`
  - `DARK`: `../cooxupe_camera_interface/cooxupe_camera_interface/acquisition_runner.cpp:359-443`

`Por que isso importa`

- O restart explícito antes do `DARK` parece ter sido pensado para descartar frames residuais do `LIGHT`.
- A pausa de 250 ms reduz a chance de corrida interna no driver/SDK exatamente no ponto onde você observava travamento.
- Mesmo que a DLL tenha sido a causa raiz principal, esse ajuste diminui a sensibilidade do sistema nessa transição.

### 4. O projeto novo separa captura e gravação sem deixar a SDK “vazar” para threads auxiliares

`Evidência no código`

- O projeto novo sobe um `SaveCore` com thread própria em `src/main.cpp:90-100`.
- `SaveCore` grava arquivos e monta thumbnail, mas não chama nenhuma função `SI_*`; o trabalho dele começa em `src/save_core.cpp:376-403` e o processamento dos chunks está em `src/save_core.cpp:405-560`.
- O pipe também roda em thread própria no novo projeto: `src/pipe_core.cpp:98-112`.
- No projeto antigo, a captura escreve RAW e thumbnail diretamente dentro dos loops de aquisição, no mesmo fluxo do runner:
  - `LIGHT`: `../cooxupe_camera_interface/cooxupe_camera_interface/acquisition_runner.cpp:283-300`
  - `DARK`: `../cooxupe_camera_interface/cooxupe_camera_interface/acquisition_runner.cpp:414-428`

`Leitura correta`

- Isso não prova sozinho a causa do travamento.
- Mas o desenho novo deixa uma regra implícita mais saudável: a thread de captura é a dona da relação com a SDK, e as threads auxiliares ficam fora da superfície de risco do driver/runtime.

### 5. A inicialização do projeto novo é bem mais enxuta

`Evidência no código`

- O projeto antigo faz seleção de device por nome, resolve canais de grabber/câmera/shutter, aplica fallback entre identificadores, usa fallback de features `Camera1.*`, e valida várias leituras: `../cooxupe_camera_interface/cooxupe_camera_interface/specsensor_device.cpp:110-439`, `../cooxupe_camera_interface/cooxupe_camera_interface/specsensor_device.cpp:720-833`.
- O projeto novo usa um caminho bem mais curto:
  - `Load`
  - `GetDeviceCount`
  - `Open(device_index)`
  - `SetString(Camera.CalibrationPack)`
  - `Initialize`
  - configurar trigger/binning/timing
  - referências: `src/capture_core.cpp:880-930`, `src/capture_core.cpp:933-1064`

`Por que isso importa`

- Menos heurística, menos fallback, menos superfície para um estado interno inconsistente antes da captura.
- Em sistemas sensíveis a driver/SDK, simplificar a inicialização costuma ser uma medida de estabilidade tão importante quanto “corrigir código”.

### 6. O projeto novo fixa a ordem de configuração da câmera

`Evidência no código`

- No novo projeto, a ordem é:
  - `Trigger.Mode = Internal`
  - `Binning`
  - `ExposureTime.Auto = false`
  - `ExposureTime`
  - `FrameRate`
- Isso está em `src/capture_core.cpp:933-1064`.
- Existem testes que protegem essa ordem e a transição entre fases:
  - `tests/test_capture_core.cpp:116-158`
  - `tests/test_capture_core.cpp:240-287`

- No projeto antigo, `ApplyStaticConfig` segue outra ordem:
  - `ExposureTime.Auto = false`
  - `FrameRate`
  - `ExposureTime`
  - `Binning`
- Referência: `../cooxupe_camera_interface/cooxupe_camera_interface/specsensor_device.cpp:813-829`

`Inferência técnica`

- Essa mudança pode alterar o estado interno do pipeline da câmera antes da captura.
- Não é prova direta da falha, mas é um ponto plausível de sensibilidade, especialmente quando frame rate, exposure e binning interagem com o tempo de leitura do sensor.

### 7. Os defaults de aquisição mudaram bastante

`Evidência no código`

- Projeto antigo:
  - `120.0 Hz`, `4 ms`, `spatial=2`, `spectral=1`
  - `../cooxupe_camera_interface/cooxupe_camera_interface/app_config.h:25-29`
- Projeto novo:
  - `310.6 Hz`, `3.1 ms`, `spatial=1`, `spectral=2`
  - `src/app_config.h:28-50`

`Leitura correta`

- Isso é uma diferença funcional real.
- Não combina com a hipótese “erro de DLL” como causa principal.
- Ainda assim, qualquer comparação de estabilidade entre os projetos precisa lembrar que o regime de aquisição também mudou.

## O que eu considero mais provável como explicação combinada

Hipótese principal:

- O projeto antigo estava mais exposto a carregar binários de runtime indesejados ou conflitantes por causa da forma como resolvia e copiava DLLs.

Hipótese secundária, mas importante:

- A transição `LIGHT -> DARK` no projeto antigo era menos robusta do que no projeto novo.

Leitura combinada:

- O travamento provavelmente era multifatorial.
- A diferença de DLL/runtime pode ter criado uma base instável.
- O ponto de maior sensibilidade operacional dessa instabilidade era exatamente o loop de `LIGHT` e a troca para `DARK`.

## Pontos de atenção para qualquer agente que mexer neste projeto

### Regras que não devem ser quebradas sem teste em hardware

- Não remover `ConfigureSpecSensorDllSearchPath()` nem voltar a depender da ordem implícita de DLL do Windows.
- Não reintroduzir cópia cega de DLLs de um `bin` genérico compartilhado sem inventário explícito do que está indo para a saída.
- Não espalhar chamadas `SI_*` por `SaveCore`, `PipeCore`, callbacks, utilitários ou threads auxiliares.
- Não remover `Acquisition.RingBuffer.Sync` antes do `LIGHT`.
- Não remover o `Stop -> espera curta -> Start` na transição para `DARK` sem validação em hardware.
- Não alterar a ordem `Trigger -> Binning -> Exposure Auto Off -> Exposure -> FrameRate` sem revalidar captura contínua.

### Regras de arquitetura

- `CaptureCore` deve continuar sendo o dono principal do handle e do fluxo da SDK.
- Threads auxiliares podem fazer I/O, fila, pipe e logging, mas não devem disputar estado do dispositivo.
- Se alguma nova thread precisar falar com a câmera, isso deve ser tratado como mudança de arquitetura de alto risco.

### Regras de build e empacotamento

- Toda mudança em `.vcxproj` deve responder explicitamente: “quais DLLs entram no diretório final e por quê?”.
- Se aparecer dependência nova de runtime/threads, ela deve ser documentada nominalmente.
- Evitar qualquer retorno a diretórios de build compartilhados e pouco controlados.

## Checklist de regressão

Se alguém mexer no fluxo de captura, precisa verificar pelo menos isto:

1. O executável continua carregando a SDK a partir do diretório esperado.
2. O comando `Acquisition.RingBuffer.Sync` continua existindo antes do `LIGHT`.
3. A sequência `CloseShutter -> Stop -> wait -> Start -> DARK` continua intacta.
4. `SaveCore` continua sem chamar a SDK.
5. A captura de múltiplos ciclos seguidos `LIGHT -> DARK` continua estável em hardware real.
6. Os testes de `tests/test_capture_core.cpp` continuam cobrindo restart antes do `DARK` e logs das fases.

## Sugestão de instrução para agentes

Ao editar captura, inicialização, build ou empacotamento, os agentes devem assumir o seguinte:

- Existe histórico de travamento associado a runtime/threads e à transição `LIGHT -> DARK`.
- Simplificação estrutural foi parte da correção.
- Alterações que aumentem fallback implícito, dependência de DLL externa ou concorrência sobre a SDK devem ser tratadas como risco alto.

## Referências principais

- `src/specsensor_api_sdk.cpp:84-106`
- `src/capture_core.cpp:367-379`
- `src/capture_core.cpp:515-731`
- `src/capture_core.cpp:880-1064`
- `src/save_core.cpp:376-560`
- `src/main.cpp:84-163`
- `tests/test_capture_core.cpp:240-287`
- `../cooxupe_camera_interface/cooxupe_camera_interface/specsensor_device.cpp:98-439`
- `../cooxupe_camera_interface/cooxupe_camera_interface/specsensor_device.cpp:720-833`
- `../cooxupe_camera_interface/cooxupe_camera_interface/acquisition_runner.cpp:213-321`
- `../cooxupe_camera_interface/cooxupe_camera_interface/acquisition_runner.cpp:359-443`
- `specsensor_cli.vcxproj:45-103`
- `../cooxupe_camera_interface/cooxupe_camera_interface/cooxupe_camera_interface.vcxproj:43-88`
