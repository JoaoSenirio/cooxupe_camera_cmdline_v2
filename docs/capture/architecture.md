# Arquitetura da Captura

## Objetivo do pipeline

O pipeline atual existe para capturar uma amostra com a câmera SpecSensor, persistir o resultado em disco e, opcionalmente, transmitir os mesmos dados para Matlab em tempo quase real.

Os objetivos arquiteturais explícitos do desenho atual são:

- manter a SDK concentrada em um único componente;
- desacoplar captura de I/O em disco e de transmissão TCP;
- preservar a ordem dos dados para todos os consumidores;
- propagar falhas de forma determinística quando save ou stream deixam de acompanhar o workflow.

## Componentes principais

### `main`

`main` é o orquestrador do processo. Ele:

- cria `ISpecSensorApi` e `CaptureCore`;
- valida o bootstrap da câmera;
- sobe `UiEngine`, `SaveCore`, `FrameStreamCore` e `PipeCore`;
- controla o estado global com `RuntimeLifecycle`;
- serializa a execução de workflows por amostra;
- converte progresso de captura e save em eventos de UI.

Na prática, `main` é o ponto onde os componentes independentes são conectados.

### `CaptureCore`

`CaptureCore` é o dono da relação com a SDK. Ele:

- inicializa a câmera;
- aplica trigger, binning, exposure e frame rate;
- aloca e libera o buffer de captura;
- executa o fluxo `LIGHT -> DARK`;
- coleta metadados do sensor;
- emite `WorkItem`s para persistência e stream;
- emite eventos de progresso para a UI.

Regra arquitetural importante:

- nenhuma outra classe do pipeline chama `SI_*` ou manipula o handle da câmera.

### `SharedWorkQueue`

`SharedWorkQueue` é a fila compartilhada entre consumidores. Ela:

- recebe `WorkItem`s publicados pelo `CaptureCore`;
- expõe FIFO independente para `Save` e `Stream`;
- só libera um slot depois que todos os consumidores ativos deram `ack`;
- entra em estado de falha global quando qualquer consumidor faz `ack(..., false, ...)`.

### `SaveCore`

`SaveCore` roda em thread própria e consome os mesmos `WorkItem`s para:

- criar a árvore de diretórios da aquisição;
- gravar `RAW` de light e dark;
- gerar `HDR` ENVI;
- gerar logs de dropped frames;
- montar um `PNG` RGB da aquisição light;
- emitir `SaveProgressEvent`.

### `FrameStreamCore`

`FrameStreamCore` roda em thread própria e consome os mesmos `WorkItem`s para:

- abrir conexão TCP com o servidor Matlab no início do job;
- serializar cada item no protocolo `FrameStreamProtocol`;
- enviar pacote e aguardar ACK por mensagem;
- fechar a conexão no fim do job.

Se o stream Matlab estiver habilitado, ele é tratado como consumidor obrigatório.

### `PipeCore`

`PipeCore` recebe comandos de texto via named pipe. O contrato aceito hoje é:

```text
CAPTURE <sample_name>\n
```

Ele converte o comando em `AcquisitionJob` e entrega para o callback de `main`.

### UI e runtime

Os componentes de UI e runtime não tocam a câmera diretamente:

- `RuntimeLifecycle` controla em que estágio o executável está.
- `WorkflowUiModel` converte progresso técnico em mensagens e percentuais de UI.
- `UiEngine` apenas publica e recebe comandos da UI Win32.

## Fluxo de bootstrap

O bootstrap normal ocorre nesta ordem:

1. `main` cria `ISpecSensorApi` e `CaptureCore`.
2. `CaptureCore.Initialize()`:
   - abre arquivo de log;
   - valida `AppConfig`;
   - conecta a câmera;
   - configura parâmetros da câmera.
3. `RuntimeLifecycle.BootstrapSucceeded()`.
4. `UiEngine.start(...)`.
5. `SaveCore.start(&work_queue)`.
6. Se `matlab_stream_enabled == true`, `FrameStreamCore.start(...)`.
7. `CaptureCore.set_work_sink(...)` e `CaptureCore.set_progress_sink(...)`.
8. `PipeCore.start(...)`.

Se o stream Matlab estiver habilitado e o `FrameStreamCore` não subir, o processo aborta na inicialização.

## Interação do `CaptureCore` com a `ISpecSensorApi`

### Fase de conexão

Durante `Initialize()`, o `CaptureCore` usa a interface da SDK nesta ordem:

1. `Load(license_path)`
2. `GetDeviceCount(...)`
3. `Open(device_index)`
4. `SetString("Camera.CalibrationPack", calibration_scp_path)`
5. `Command("Initialize")`

Essa fase estabelece o handle e deixa a câmera pronta para configuração.

### Fase de configuração

A ordem de configuração atual é intencional e protegida por testes:

1. descobrir o enum `Internal` em `Camera.Trigger.Mode`;
2. `SetEnumIndex("Camera.Trigger.Mode", internal_index)`;
3. validar readback com `GetEnumIndex("Camera.Trigger.Mode", ...)`;
4. `SetEnumIndex("Camera.Binning.Spatial", ...)`;
5. `SetEnumIndex("Camera.Binning.Spectral", ...)`;
6. `SetBool("Camera.ExposureTime.Auto", false)`;
7. `SetFloat("Camera.ExposureTime", exposure_ms)`;
8. `SetFloat("Camera.FrameRate", frame_rate_hz)`;
9. ler de volta exposure e frame rate com `GetFloat(...)`.

O `CaptureCore` também registra `Camera.Image.ReadoutTime` antes do binning, depois do binning e depois da aplicação de timing, quando a feature existe.

### Fase de snapshot

Antes de capturar uma amostra, `CaptureCore` coleta um `SensorSnapshot` com:

- geometria da imagem;
- `frame_size_bytes`;
- `byte_depth`;
- `frame_rate_hz` e `exposure_ms` efetivos;
- binning aplicado;
- `sensor_id`;
- offsets da janela de aquisição;
- temperatura VNIR;
- tabela de comprimentos de onda;
- tabela de `fwhm`;
- caminho do calibration pack.

Esse snapshot alimenta tanto a persistência em disco quanto o payload de início do stream Matlab.

### Fase de captura

O fluxo de captura por amostra é:

1. `CreateBuffer(frame_size_bytes, &buffer)`
2. emitir `BeginJob` para a fila de trabalho
3. `Command("Acquisition.Start")`
4. `Command("Acquisition.RingBuffer.Sync")`
5. `Command("Camera.OpenShutter")`
6. loop `LIGHT` com `Wait(...)`
7. `Command("Camera.CloseShutter")`
8. `Command("Acquisition.Stop")`
9. esperar `250 ms`
10. `Command("Acquisition.Start")`
11. loop `DARK` com `Wait(...)`
12. `Command("Acquisition.Stop")`
13. `DisposeBuffer(buffer)`
14. emitir `EndJob`

Observações importantes:

- `LIGHT` dura `capture_seconds`.
- `DARK` captura `dark_frames`.
- o restart entre `LIGHT` e `DARK` é explícito para descartar frames residuais.
- `CaptureCore` detecta gaps de `frame_number` e acumula incidentes de drop.

### Fase de shutdown

No encerramento normal:

1. `Close()`
2. `Unload()`

`Shutdown()` é idempotente do ponto de vista do componente.

## Fluxo de comando até captura

O fluxo de controle para iniciar uma amostra é:

1. `PipeCore` recebe uma linha de texto.
2. `PipeCore` aceita apenas `CAPTURE <sample_name>`.
3. O callback em `main` valida se:
   - não há shutdown solicitado;
   - não há estado fatal;
   - não há workflow ativo;
   - não há job pendente.
4. Se aceito, `pending_job` é preenchido e o loop principal acorda.
5. `main` marca `workflow_busy = true` e chama `CaptureCore.CaptureSample(...)`.

O desenho atual só aceita uma amostra por vez.

## Emissão de `WorkItem`s

O `CaptureCore` converte a captura em uma sequência ordenada de `WorkItem`s:

1. `BeginJob`
2. zero ou mais `LightChunk`
3. zero ou mais `DarkChunk`
4. `EndJob`

Esses itens preservam a ordem lógica do workflow para os dois consumidores.

### Chunking atual

O tamanho do chunk é calculado por `DetermineChunkFrameTarget(frame_size_bytes)`:

- alvo nominal: `256` frames por chunk;
- teto nominal de bytes por chunk: `512 MiB`;
- se `256 * frame_size_bytes` exceder o teto, o número de frames é reduzido;
- o mínimo efetivo é `16` frames por chunk nessa redução.

O chunk carrega:

- `bytes` compartilhados por `shared_ptr`;
- `frame_count`;
- `first_frame_number`;
- `last_frame_number`.

O payload TCP atual não transmite `first_frame_number` nem `last_frame_number`; eles continuam disponíveis apenas dentro do processo.

## Semântica da `SharedWorkQueue`

### Consumidores

Hoje existem dois consumidores possíveis:

- `Save`
- `Stream`

Eles são ativados por máscara:

- `kConsumerSaveMask = 0x01`
- `kConsumerStreamMask = 0x02`

### Publicação

`publish(item, timeout)`:

- bloqueia enquanto a fila estiver cheia;
- falha por timeout, `closed_` ou `failed_`;
- anexa um `sequence` monotônico por item.

### Pop por consumidor

Cada consumidor possui um cursor lógico próprio:

- o `Save` pode ter popado um item que o `Stream` ainda não popou;
- a ordem FIFO é preservada por consumidor.

### ACK e liberação de slot

Um item só sai da frente da fila quando `pending_mask == 0`, ou seja:

- todos os consumidores ativos daquele item já confirmaram sucesso.

Consequência:

- quando o stream Matlab está ativo, a persistência em disco sozinha não libera espaço;
- um consumidor lento de stream faz backpressure direto sobre a captura.

### Falha global

Se qualquer consumidor fizer `ack(lease, false, reason)`:

- `failed_` vira `true`;
- a razão da falha é registrada;
- novos `publish(...)` falham;
- novos `pop(...)` passam a falhar.

Esse é o mecanismo que propaga falhas de save ou stream para o processo principal.

## Capacidade efetiva da fila

Existe uma distinção importante entre configuração e runtime real:

- quando `matlab_stream_enabled == false`, `main` cria a `SharedWorkQueue` com capacidade `save_queue_capacity`;
- quando `matlab_stream_enabled == true`, `main` força a capacidade da fila compartilhada para `3`.

Logo:

- com Matlab ativo, a fila compartilhada real não usa `matlab_stream_queue_capacity`;
- o throughput do workflow passa a depender de uma janela pequena de três itens pendentes.

## Persistência em disco no fluxo geral

O `SaveCore` consome a mesma sequência `BeginJob -> chunks -> EndJob` e gera:

- diretório da aquisição: `CAMERA_TIMESTAMP_SAMPLE`
- subdiretório `capture/`
- `RAW` light
- `RAW` dark
- `HDR` ENVI light
- `HDR` ENVI dark
- log de dropped frames light
- log de dropped frames dark
- `PNG` RGB derivado apenas da fase light

A escolha das bandas RGB segue esta regra:

- se `SensorSnapshot.wavelengths_nm` estiver preenchido, o save escolhe a banda mais próxima de cada comprimento de onda alvo;
- caso contrário, ele usa quartis aproximados da dimensão espectral.

## Propagação de falhas

### Falhas de captura

Se o `CaptureCore` falha em:

- inicializar a câmera;
- alocar buffer;
- chamar `Wait(...)`;
- publicar `WorkItem`;
- finalizar a sequência de captura,

ele retorna `AcquisitionSummary.pass = false` e `main` entra em estado fatal.

### Falhas de save

Se o `SaveCore` falhar ao processar qualquer item:

- faz `ack(false, "save consumer failed")`;
- derruba a `SharedWorkQueue`;
- o pipeline deixa de aceitar novos itens.

### Falhas de stream Matlab

Se o `FrameStreamCore` falhar em:

- conectar;
- serializar item;
- enviar bytes;
- receber ACK válido do Matlab,

ele faz `ack(false, "stream consumer failed")` e derruba a fila inteira.

### Stream em plataforma não Windows

Quando compilado fora de Windows:

- o worker de stream não transmite nada;
- no primeiro item de stream ele sinaliza falha;
- portanto, com stream Matlab habilitado, o workflow não é funcional fora de Windows.

## Relação com a UI

O `CaptureCore` publica `CaptureProgressEvent` e o `SaveCore` publica `SaveProgressEvent`.

`main` converte esses eventos em `UiEvent` via `WorkflowUiModel`:

- captura ocupa as faixas de progresso até o fim da fase de aquisição;
- save consome a parte final da barra;
- um `JobFinished` com falha leva o processo a estado fatal;
- um `JobFinished` bem sucedido libera o runtime para o próximo workflow.

## Fonte prática de verdade

Os contratos descritos aqui são reforçados pelos testes:

- `tests/test_capture_initialize.cpp`
- `tests/test_capture_workflow.cpp`
- `tests/test_shared_work_queue.cpp`
- `tests/test_save_core_stream.cpp`
- `tests/test_frame_stream_protocol.cpp`
