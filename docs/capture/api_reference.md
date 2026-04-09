# Referência de Classes, Métodos e Contratos

## Como ler este arquivo

Esta referência está organizada por responsabilidade lógica, não por arquivo-fonte.

Para cada classe ou tipo, o texto resume:

- propósito;
- estado principal;
- quem chama;
- thread de execução;
- métodos relevantes;
- falhas ou efeitos colaterais importantes.

## API da câmera

### `ISpecSensorApi`

**Propósito**

Abstração da SDK da câmera. Isola o código de captura da implementação concreta baseada em `SI_*`.

**Implementação atual**

- `SpecSensorApiSdk` em `src/specsensor_api_sdk.cpp`

**Chamado por**

- `CaptureCore` apenas

**Thread**

- thread principal do workflow de captura

**Métodos principais**

- `Load(const std::wstring& license_path)`
  Carrega a SDK e prepara o runtime da biblioteca.
- `Unload()`
  Descarrega a SDK.
- `GetDeviceCount(std::int64_t* count)`
  Lê a quantidade de devices.
- `Open(int device_index)`
  Abre o device selecionado.
- `Close()`
  Fecha o device aberto.
- `Command(const std::wstring& feature)`
  Executa comandos como `Initialize`, `Acquisition.Start` e `Camera.OpenShutter`.
- `SetBool`, `SetFloat`, `SetString`, `SetEnumIndex`
  Aplicam configuração.
- `GetInt`, `GetBool`, `GetFloat`, `GetEnumIndex`, `GetEnumCount`, `GetEnumStringByIndex`
  Fazem readback e descoberta de features.
- `CreateBuffer(std::int64_t size_bytes, void** buffer)`
  Aloca o buffer usado por `Wait`.
- `DisposeBuffer(void* buffer)`
  Libera o buffer.
- `Wait(std::uint8_t* buffer, std::int64_t* frame_size, std::int64_t* frame_number, std::int64_t timeout_ms)`
  Espera um frame e preenche buffer, tamanho e número do frame.
- `GetErrorString(int code) const`
  Traduz códigos da SDK.

**Falhas relevantes**

- qualquer erro propagado pela SDK é tratado pelo `CaptureCore` como falha operacional do workflow;
- fora de `CaptureCore`, o restante do sistema não deve depender dessa interface.

## Núcleo de captura

### `CaptureCore`

**Propósito**

Executar a captura da amostra e converter frames em `WorkItem`s e eventos de progresso.

**Estado principal**

- `config_`
- `api_`
- `initialized_`
- `shutdown_done_`
- `stop_requested_`
- `next_job_id_`
- `work_sink_`
- `progress_sink_`

**Chamado por**

- `main`

**Thread**

- thread principal do workflow

**Métodos principais**

- `Initialize()`
  Valida config, conecta a câmera e aplica a configuração.
- `CaptureSample(const AcquisitionJob& job, AcquisitionSummary* summary)`
  Executa o workflow completo `LIGHT -> DARK`, emite chunks e retorna resumo.
- `Shutdown()`
  Fecha device e descarrega a SDK.
- `set_work_sink(std::function<bool(WorkItem)> work_sink)`
  Registra o callback que publica `WorkItem`s.
- `set_progress_sink(std::function<void(const CaptureProgressEvent&)> progress_sink)`
  Registra o callback de progresso para UI.
- `RequestStop()`
  Sinaliza parada cooperativa.
- `StopRequested() const`
  Permite consulta do estado de parada.
- `LogInfo(...)` e `LogError(...)`
  Escrevem em stdout/stderr e no arquivo de log.

**Métodos internos importantes**

- `ConnectCamera()`
  Chama `Load`, `GetDeviceCount`, `Open`, aplica calibration pack e executa `Initialize`.
- `ConfigureCameraParameters()`
  Aplica trigger, binning, exposure e frame rate.
- `FillSensorSnapshot(SensorSnapshot* snapshot)`
  Coleta metadados do sensor usados por save e stream.
- `EmitWorkItem(WorkItem item, AcquisitionSummary* summary)`
  Publica `WorkItem` e converte falha de fila em erro de captura.

**Efeitos colaterais**

- produz logs;
- chama a SDK;
- gera diretivas de save/stream por `WorkItem`;
- atualiza UI indiretamente via eventos de progresso.

**Falhas relevantes**

- falha de SDK;
- timeout ou indisponibilidade do `work_sink_`;
- parada solicitada pelo usuário;
- inconsistência de `frame_size`.

## Persistência

### `SaveCore`

**Propósito**

Persistir o workflow em disco e emitir progresso de save.

**Estado principal**

- `started_`
- `active_`
- `work_queue_`
- `progress_sink_`
- thread interna `worker_`

`active_` concentra o job aberto:

- paths de saída;
- arquivos light/dark;
- snapshot do sensor;
- índices RGB resolvidos;
- buffers acumulados para o `PNG`;
- contadores de bytes.

**Chamado por**

- iniciado por `main`
- consome itens da `SharedWorkQueue`

**Thread**

- worker thread própria

**Métodos principais**

- `start(SharedWorkQueue* work_queue)`
  Sobe o worker.
- `stop()`
  Para o worker e aguarda `join`.
- `set_progress_sink(...)`
  Registra callback de progresso.

**Métodos internos importantes**

- `handle_begin(const WorkItem& item)`
  Cria diretórios, arquivos e estado do job.
- `handle_chunk(const WorkItem& item)`
  Grava bytes em `RAW`, acumula preview RGB e emite progresso.
- `handle_end(const WorkItem& item)`
  Fecha arquivos, escreve `HDR`, logs de drop, `PNG` e emite `JobFinished`.
- `write_hdr(...)`
  Gera cabeçalho ENVI.
- `write_drop_log(...)`
  Gera log de dropped frames.
- `write_rgb_png()`
  Gera `PNG` RGB a partir da fase light.

**Efeitos colaterais**

- cria diretórios;
- grava arquivos;
- consome memória para os canais RGB acumulados do `PNG`.

**Falhas relevantes**

- path inválido;
- falha de abertura ou escrita de arquivo;
- mismatch de `job_id`;
- chunk com tamanho incompatível com `frame_size_bytes`.

## Stream Matlab

### `FrameStreamCore`

**Propósito**

Transmitir os `WorkItem`s para Matlab via TCP.

**Estado principal**

- `started_`
- `work_queue_`
- `host_`
- `port_`
- `connect_timeout_ms_`
- `send_timeout_ms_`
- `connection_`

**Chamado por**

- iniciado por `main`
- consome itens da `SharedWorkQueue`

**Thread**

- worker thread própria

**Métodos principais**

- `start(SharedWorkQueue* work_queue, const std::string& host, int port, int connect_timeout_ms, int send_timeout_ms)`
  Configura o destino e sobe o worker.
- `stop()`
  Para o worker e fecha a conexão.

**Métodos internos importantes**

- `worker_loop()`
  Consome `Lease`s do consumidor `Stream`.
- `handle_item(const WorkItem& item)`
  Decide conexão por tipo do item.
- `ensure_connected()`
  Abre o socket TCP.
- `send_item(const WorkItem& item)`
  Serializa, envia header/payload e aguarda ACK.
- `receive_ack()`
  Lê e valida o ACK do Matlab.
- `reset_connection()`
  Fecha o socket atual.

**Falhas relevantes**

- não funciona fora de Windows;
- qualquer erro de conexão, envio ou ACK derruba a fila compartilhada;
- o stream é síncrono por item e por ACK.

## Orquestração de entrada

### `PipeCore`

**Propósito**

Receber pedidos de captura via named pipe.

**Estado principal**

- `pipe_name_`
- `callback_`
- `started_`
- `wake_for_shutdown_`
- `pending_line_`

**Chamado por**

- iniciado por `main`

**Thread**

- worker thread própria

**Métodos principais**

- `start(const std::string& pipe_name, JobCallback callback)`
  Sobe o servidor de pipe.
- `stop()`
  Para o worker e acorda uma espera bloqueante.

**Métodos internos importantes**

- `worker_loop()`
  Aceita conexões e lê bytes do pipe.
- `process_text(const std::string& text_chunk, std::uint64_t connection_id)`
  Monta linhas e valida o comando textual.

**Contrato de entrada**

- aceita apenas `CAPTURE <sample_name>`
- exige linha terminada por `\n`

**Falhas relevantes**

- comando inválido;
- job rejeitado por workflow ocupado;
- falhas de API do named pipe.

## Fila compartilhada

### `SharedWorkQueue`

**Propósito**

Distribuir os mesmos `WorkItem`s para `Save` e `Stream`, preservando ordem por consumidor e exigindo ACK de todos.

**Estado principal**

- `capacity_`
- `consumer_mask_`
- `entries_`
- `next_sequence_`
- `next_publish_sequence_`
- `closed_`
- `failed_`
- `failure_reason_`

**Chamado por**

- `CaptureCore` publica via `work_sink_`
- `SaveCore` consome como `SharedWorkConsumer::Save`
- `FrameStreamCore` consome como `SharedWorkConsumer::Stream`

**Thread**

- multi-threaded, protegido por mutex e condition variable

**Métodos principais**

- `publish(WorkItem item, std::chrono::milliseconds timeout)`
  Publica um item e espera espaço até o timeout.
- `pop(SharedWorkConsumer consumer, Lease* out)`
  Faz pop lógico para um consumidor específico.
- `ack(const Lease& lease, bool success, const std::string& failure_reason = {})`
  Marca sucesso ou falha do consumidor.
- `close()`
  Fecha a fila para novas publicações.
- `failed() const`
  Informa se houve falha global.
- `failure_reason() const`
  Retorna a razão registrada.
- `size() const`
  Retorna o número de entradas ainda não aposentadas.

### `SharedWorkQueue::Lease`

**Propósito**

Representar a posse temporária de um item por um consumidor.

**Campos principais**

- `consumer`
- `sequence`
- `item`

**Regra importante**

- o `Lease` precisa ser confirmado por `ack(...)`; sem isso, o slot não é liberado.

## Runtime e UI

### `RuntimeLifecycle`

**Propósito**

Modelar o estado macro do executável.

**Métodos principais**

- `BootstrapSucceeded()`
- `BootstrapFailed()`
- `CaptureStarted()`
- `WorkflowFinished(bool success)`
- `FatalErrorOccurred()`
- `ShutdownRequested()`
- `background_workers_may_start() const`
- `pipe_should_run() const`

### `WorkflowUiModel`

**Propósito**

Converter progresso técnico em `UiEvent`.

**Métodos principais**

- `OnCaptureStarted(...)`
- `OnCaptureProgress(...)`
- `OnCaptureFinished(...)`
- `OnSaveProgress(...)`
- `MakeFatalError(...)`
- `MakeHideEvent() const`

### `UiEngine`

**Propósito**

Publicar `UiEvent`s e receber `UiCommand`s da UI Win32.

**Métodos principais**

- `start(CommandSink command_sink)`
- `stop()`
- `publish(const UiEvent& event)`

## Tipos de contrato

### `AcquisitionJob`

**Propósito**

Representar um pedido de captura.

**Campos principais**

- `sample_name`

### `AcquisitionSummary`

**Propósito**

Resumir o resultado final da captura.

**Campos principais**

- `sample_name`
- `light_buffers`
- `dark_buffers`
- `total_buffers`
- `light_drop_incidents`
- `dark_drop_incidents`
- `light_dropped_frames`
- `dark_dropped_frames`
- `last_frame_number`
- `pass`
- `sdk_error`
- `message`

### `SensorSnapshot`

**Propósito**

Capturar os metadados do sensor e da geometria do frame no início do workflow.

**Campos principais**

- `image_width`
- `image_height`
- `frame_size_bytes`
- `byte_depth`
- `frame_rate_hz`
- `exposure_ms`
- `binning_spatial`
- `binning_spectral`
- `sensor_id`
- `calibration_pack`
- `acquisitionwindow_left`
- `acquisitionwindow_top`
- `has_vnir_temperature`
- `vnir_temperature`
- `wavelengths_nm`
- `fwhm_nm`

### `SaveEventBegin`

**Propósito**

Payload de início do job para save e stream.

**Campos principais**

- `sample_name`
- `camera_name`
- `output_dir`
- `timestamp_tag`
- `expected_light_frames`
- `expected_dark_frames`
- `rgb_wavelength_nm[3]`
- `sensor`
- `acquisition_date_utc`
- `light_start_time_utc`

### `SaveEventEnd`

**Propósito**

Payload final com resultado consolidado do workflow.

**Campos principais**

- `success`
- `sdk_error`
- `message`
- `light_frames`
- `dark_frames`
- `light_drop_incidents`
- `dark_drop_incidents`
- `light_dropped_frames`
- `dark_dropped_frames`
- timestamps UTC de início e fim das fases

### `SaveEventChunk`

**Propósito**

Tipo legado/intermediário ainda definido em `types.h`.

**Estado atual**

- o fluxo ativo entre `CaptureCore`, `SharedWorkQueue`, `SaveCore` e `FrameStreamCore` usa `WorkItemChunk`;
- `SaveEventChunk` não participa do transporte real dos chunks no runtime atual.

### `WorkItemChunk`

**Propósito**

Representar um bloco de frames light ou dark.

**Campos principais**

- `bytes`
- `frame_count`
- `first_frame_number`
- `last_frame_number`

### `WorkItem`

**Propósito**

Unificar `Begin`, `Chunk` e `End` em um único tipo transportado pela fila.

**Campos principais**

- `type`
- `job_id`
- `begin`
- `chunk`
- `end`

### `CaptureProgressEvent`

**Propósito**

Informar progresso de captura para a UI.

**Campos principais**

- `type`
- `sample_name`
- `phase`
- `light_frames_captured`
- `dark_frames_captured`
- `dark_frames_target`
- `frame_size_bytes`
- `capture_elapsed_seconds`
- `phase_elapsed_seconds`
- `capture_target_seconds`
- `estimated_frame_rate_hz`
- `success`
- `sdk_error`
- `message`

### `SaveProgressEvent`

**Propósito**

Informar progresso de persistência para a UI.

**Campos principais**

- `type`
- `job_id`
- `sample_name`
- `bytes_written`
- `total_bytes`
- `bytes_per_second`
- `success`
- `message`

### `UiEvent`

**Propósito**

Contrato publicado para a UI Win32.

**Campos principais**

- `type`
- `stage`
- `title`
- `detail`
- `progress_percent`
- `eta_seconds`
- `auto_hide_delay_ms`

### `UiCommand`

**Propósito**

Contrato recebido da UI para o processo principal.

**Campos principais**

- `type`

## Enums relevantes

### `WorkItemType`

- `BeginJob`
- `LightChunk`
- `DarkChunk`
- `EndJob`

### `SharedWorkConsumer`

- `Save`
- `Stream`

### `CapturePhase`

- `Light`
- `Dark`

### `CaptureProgressType`

- `CaptureStarted`
- `CaptureProgress`
- `CaptureFinished`

### `SaveProgressType`

- `JobStarted`
- `BytesWritten`
- `JobFinished`
