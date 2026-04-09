# Valores Padrão, Constantes e Lacunas Atuais

## Defaults de `AppConfig`

Os valores abaixo vêm de `MakeDefaultConfig()`.

### Câmera e SDK

| Campo | Default atual | Observação |
| --- | --- | --- |
| `license_path` | `C:/Users/Public/Documents/Specim/SpecSensor.lic` | path local da licença |
| `device_index` | `10` | depende do ambiente |
| `calibration_scp_path` | `E:/Calibrations/3210495_20220310_calpack.scp` | path local do calibration pack |
| `camera_name` | `FX10` | usado em nomes de arquivos |

### Timing e geometria operacional

| Campo | Default atual | Observação |
| --- | --- | --- |
| `exposure_ms` | `3.1` | aplicado por `CaptureCore` |
| `frame_rate_hz` | `310.6` | aplicado por `CaptureCore` |
| `binning_spatial` | `1` | validado para `1, 2, 4, 8` |
| `binning_spectral` | `2` | validado para `1, 2, 4, 8` |

### Captura

| Campo | Default atual | Observação |
| --- | --- | --- |
| `capture_seconds` | `70` | duração da fase `LIGHT` |
| `dark_frames` | `50` | frames da fase `DARK` |
| `wait_timeout_ms` | `1000` | timeout de `Wait(...)` |
| `min_buffers_required` | `5000` | hoje é validado, mas não participa da decisão final de `pass` |

### Persistência

| Campo | Default atual | Observação |
| --- | --- | --- |
| `output_dir` | `R:/Amostras_Soja_26_03_2026` | raiz de saída |
| `rgb_wavelength_nm[0]` | `610` | alvo do canal vermelho |
| `rgb_wavelength_nm[1]` | `534` | alvo do canal verde |
| `rgb_wavelength_nm[2]` | `470` | alvo do canal azul |
| `save_queue_capacity` | `200` | usada quando Matlab está desabilitado |
| `save_block_frames` | `64` | validado, mas não usado pelo fluxo atual |
| `save_queue_push_timeout_ms` | `2000` | timeout de publicação na fila |
| `log_file_path` | `C:/SpecimOutput/specsensor_cli.log` | arquivo de log do `CaptureCore` |

### Pipe

| Campo | Default atual | Observação |
| --- | --- | --- |
| `pipe_name` | `\\\\.\\pipe\\specsensor_sample_pipe` | contrato de entrada textual |

### Stream Matlab

| Campo | Default atual | Observação |
| --- | --- | --- |
| `matlab_stream_enabled` | `false` | stream opcional por config |
| `matlab_stream_host` | `127.0.0.1` | servidor TCP Matlab |
| `matlab_stream_port` | `55001` | porta TCP |
| `matlab_stream_connect_timeout_ms` | `200` | timeout de conexão |
| `matlab_stream_send_timeout_ms` | `200` | timeout de envio e leitura de ACK |
| `matlab_stream_queue_capacity` | `8` | existe na config, mas não define a capacidade real da fila compartilhada |

## Regras de validação relevantes

`ValidateConfig(...)` exige, entre outros pontos:

- paths obrigatórios não vazios;
- `device_index >= 0`;
- `exposure_ms > 0`;
- `frame_rate_hz > 0`;
- `capture_seconds > 0`;
- `dark_frames >= 0`;
- `save_queue_capacity > 0`;
- `save_block_frames > 0`;
- `save_queue_push_timeout_ms > 0`;
- `matlab_stream_host` não vazio;
- `matlab_stream_port` entre `1` e `65535`;
- `matlab_stream_connect_timeout_ms > 0`;
- `matlab_stream_send_timeout_ms > 0`;
- `matlab_stream_queue_capacity > 0`.

## Constantes do protocolo TCP

### Header e ACK

| Constante | Valor |
| --- | --- |
| `FrameStreamProtocol::kHeaderBytes` | `12` |
| `FrameStreamProtocol::kAckBytes` | `8` |
| `FrameStreamProtocol::kVersion` | `1` |
| `FrameStreamProtocol::kMagic` | `SSFR` |
| `FrameStreamProtocol::kAckMagic` | `SSFA` |

### Tipos de mensagem

| Constante | Valor |
| --- | --- |
| `MessageType::Begin` | `1` |
| `MessageType::LightBlock` | `2` |
| `MessageType::DarkBlock` | `3` |
| `MessageType::End` | `4` |

## Constantes de runtime da captura

Estas constantes vivem hoje em `capture_core.cpp`.

| Constante | Valor | Papel |
| --- | --- | --- |
| `kAppStoppedByUser` | `-30000` | parada cooperativa do usuário |
| `kAppSaveQueueError` | `-30001` | falha de publicação na fila |
| `kAppInvalidFrameSize` | `-30002` | frame com tamanho inesperado |
| `kAppSnapshotError` | `-30003` | falha ao coletar snapshot |
| `kMinRestartDelay` | `250 ms` | espera entre `LIGHT` e `DARK` |
| `kFloatReadbackTolerance` | `1e-6` | tolerância de readback numérico |
| `kChunkTargetFrames` | `256` | alvo nominal de frames por chunk |
| `kChunkMaxBytes` | `512 MiB` | teto nominal de bytes por chunk |

## Regras derivadas relevantes

### Tamanho de chunk

O número real de frames por chunk é calculado assim:

- alvo inicial: `256` frames;
- se `256 * frame_size_bytes <= 512 MiB`, o chunk fica com `256` frames;
- caso contrário, o valor é reduzido para caber no teto;
- o mínimo resultante é `16` frames.

### Capacidade real da `SharedWorkQueue`

A capacidade da fila compartilhada não é sempre a mesma da config:

- sem Matlab: `save_queue_capacity`
- com Matlab: `3`

Logo, o stream Matlab usa uma fila efetiva muito menor do que a config sugere.

### Máscaras de consumidor

| Constante | Valor |
| --- | --- |
| `SharedWorkQueue::kConsumerSaveMask` | `0x01` |
| `SharedWorkQueue::kConsumerStreamMask` | `0x02` |

## Mapeamentos de tipo relevantes

### `byte_depth` para ENVI em `SaveCore`

| `byte_depth` | `data type` ENVI |
| --- | --- |
| `1` | `1` |
| `2` | `12` |
| `4` | `4` |

### `byte_depth` para Matlab

| `byte_depth` | classe Matlab |
| --- | --- |
| `1` | `uint8` |
| `2` | `uint16` |
| `4` | `uint32` |

## Lacunas e parâmetros que hoje não mudam o runtime real

### `save_block_frames`

- É validado em `ValidateConfig(...)`.
- Tem default `64`.
- Hoje não é usado nem pelo `CaptureCore`, nem pelo `SaveCore`, nem pela fila compartilhada.

### `SaveCore(queue_capacity, enqueue_timeout_ms)`

- O construtor recebe `queue_capacity` e `enqueue_timeout_ms`.
- Na implementação atual, ambos são descartados.
- O comportamento real do save depende da `SharedWorkQueue` externa, não desses membros.

### `FrameStreamCore(queue_capacity)`

- O construtor recebe `queue_capacity`.
- Na implementação atual, o argumento é descartado.
- O stream depende da `SharedWorkQueue` criada em `main`, não de uma fila interna própria.

### `matlab_stream_queue_capacity`

- É validado em `ValidateConfig(...)`.
- Tem default `8`.
- O valor é passado ao construtor de `FrameStreamCore`, mas não altera a capacidade efetiva da fila.
- Com Matlab ativo, a `SharedWorkQueue` é criada com capacidade fixa `3`.

### `min_buffers_required`

- É validado em `ValidateConfig(...)`.
- Tem default `5000`.
- No código atual, `CaptureCore` não usa esse campo para decidir `AcquisitionSummary.pass`.
- O critério real de `pass` hoje é:
  - `sdk_error == 0`
  - `light_buffers > 0`
  - `dark_buffers == config.dark_frames`

## Outras observações operacionais

- `matlab_stream_send_timeout_ms` define tanto `SO_SNDTIMEO` quanto `SO_RCVTIMEO`.
- O protocolo envia `double`s do payload `Begin` como bytes crus do host.
- O stream é obrigatório quando habilitado; não existe modo "best effort".
- O servidor Matlab atual acumula cubos completos em memória para o job inteiro.
