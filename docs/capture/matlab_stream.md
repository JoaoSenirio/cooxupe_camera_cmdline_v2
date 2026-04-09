# Fluxo TCP e Integração com Matlab

## Objetivo

O fluxo Matlab permite que o processo C++ transmita para um servidor TCP em Matlab os mesmos dados de captura usados pelo `SaveCore`.

O caminho atual é:

```text
CaptureCore
  -> WorkItem
  -> SharedWorkQueue
  -> FrameStreamCore
  -> TCP
  -> SpecSensorRgbStreamServer.m
```

Esse fluxo é opcional por configuração, mas quando habilitado passa a ser obrigatório para o workflow terminar com sucesso.

## Ativação

O stream Matlab depende de:

- `AppConfig.matlab_stream_enabled == true`
- `FrameStreamCore.start(...)` bem sucedido durante o bootstrap
- disponibilidade do servidor TCP Matlab no host e porta configurados

Se o stream estiver habilitado e o worker não conseguir iniciar, o processo aborta ainda no bootstrap.

## Papel do `FrameStreamCore`

`FrameStreamCore` consome `WorkItem`s da `SharedWorkQueue` usando o consumidor `Stream`.

Regras do lifecycle atual:

- `BeginJob`: fecha qualquer conexão anterior e abre uma nova conexão TCP.
- `LightChunk` e `DarkChunk`: exigem conexão já ativa.
- `EndJob`: transmite o fim do job e fecha a conexão.

Na prática, existe uma conexão TCP por job.

## Thread e backpressure

`FrameStreamCore` roda em thread própria, mas não é assíncrono em relação ao protocolo.

Para cada item:

1. serializa o item;
2. envia `header`;
3. envia payload inline, se existir;
4. envia payload externo, se existir;
5. bloqueia até receber um ACK.

Como a `SharedWorkQueue` só libera o slot depois do ACK do save e do stream:

- o stream Matlab aplica backpressure real sobre a captura;
- um Matlab lento reduz o throughput máximo do workflow;
- uma falha no Matlab derruba a fila inteira.

## Timeouts e conexão

### Conexão

Ao abrir a conexão, o código:

- resolve o host com `getaddrinfo`;
- cria socket TCP IPv4;
- faz `connect` não bloqueante;
- espera escrita pronta via `select` com `matlab_stream_connect_timeout_ms`.

### Envio e recebimento

Depois da conexão:

- o socket volta ao modo bloqueante;
- `SO_SNDTIMEO` recebe `matlab_stream_send_timeout_ms`;
- `SO_RCVTIMEO` também recebe `matlab_stream_send_timeout_ms`.

Observação:

- o nome do campo é `send_timeout_ms`, mas ele é usado tanto para envio quanto para leitura do ACK.

## Condições de falha do stream

O `FrameStreamCore` considera falha fatal quando ocorre qualquer uma destas condições:

- falha ao conectar no início do job;
- falha ao serializar um `WorkItem`;
- falha parcial ou total em `send(...)`;
- falha em `recv(...)` do ACK;
- ACK inválido;
- ACK com `status != 0`.

Nesses casos o consumidor faz:

```text
ack(lease, false, "stream consumer failed")
```

e toda a `SharedWorkQueue` entra em estado de falha.

## Protocolo binário atual

### Convenções gerais

- `magic` de mensagem: `SSFR`
- `magic` de ACK: `SSFA`
- versão atual: `1`
- inteiros: little-endian
- `double`: bytes crus do host, que hoje assumem little-endian IEEE-754 no runtime Windows x86_64

Observação importante:

- os `double`s do payload `Begin` não são normalizados para um formato independente de plataforma; hoje o emissor e o consumidor assumem a mesma endianness.

### Tipos de mensagem

| Valor | Tipo lógico | Origem |
| --- | --- | --- |
| `1` | `Begin` | `WorkItemType::BeginJob` |
| `2` | `LightBlock` | `WorkItemType::LightChunk` |
| `3` | `DarkBlock` | `WorkItemType::DarkChunk` |
| `4` | `End` | `WorkItemType::EndJob` |

### Header

Tamanho fixo: `12` bytes.

| Offset | Tamanho | Tipo | Campo | Valor atual |
| --- | --- | --- | --- | --- |
| `0` | `4` | `char[4]` | `magic` | `SSFR` |
| `4` | `2` | `uint16` | `version` | `1` |
| `6` | `2` | `uint16` | `message_type` | `1..4` |
| `8` | `4` | `uint32` | `payload_length` | bytes do payload |

Não existe metadata separada no protocolo atual. O receptor lê apenas `header + payload`.

### ACK

Tamanho fixo: `8` bytes.

| Offset | Tamanho | Tipo | Campo | Significado |
| --- | --- | --- | --- | --- |
| `0` | `4` | `char[4]` | `magic` | `SSFA` |
| `4` | `2` | `uint16` | `version` | deve ser `1` |
| `6` | `2` | `uint16` | `status` | `0 = sucesso`, `1 = falha` |

O ACK é obrigatório para toda mensagem enviada.

## Payload `Begin`

O payload `Begin` descreve a geometria do job e os comprimentos de onda necessários para o lado Matlab alocar memória e montar o preview RGB.

Parte fixa: `36` bytes.

| Offset | Tamanho | Tipo | Campo |
| --- | --- | --- | --- |
| `0` | `4` | `uint32` | `image_width` |
| `4` | `4` | `uint32` | `band_count` |
| `8` | `4` | `uint32` | `byte_depth` |
| `12` | `4` | `uint32` | `frame_size_bytes` |
| `16` | `4` | `uint32` | `expected_light_frames` |
| `20` | `4` | `uint32` | `expected_dark_frames` |
| `24` | `4` | `uint32` | `rgb_target_red_nm` |
| `28` | `4` | `uint32` | `rgb_target_green_nm` |
| `32` | `4` | `uint32` | `rgb_target_blue_nm` |

Parte variável:

- a partir do offset `36`, vem `band_count * 8` bytes;
- cada banda é um `double` com o comprimento de onda em nanômetros.

O payload `Begin` não transmite:

- `sample_name`;
- `camera_name`;
- `job_id`;
- timestamps;
- `first_frame_number` ou `last_frame_number`.

Esses dados continuam internos ao processo C++.

## Payload `LightBlock` e `DarkBlock`

Os blocos light e dark carregam apenas os bytes crus do chunk.

Contrato atual:

- o payload é exatamente `chunk.bytes`;
- o receptor deduz `frameCount = payload_length / frame_size_bytes`;
- o payload segue a mesma ordem do buffer capturado, que é persistida em disco como `RAW`.

Não há metadata inline com:

- `frame_count`;
- `first_frame_number`;
- `last_frame_number`.

Esses campos existem no `WorkItemChunk`, mas não são enviados no protocolo.

## Payload `End`

Tamanho fixo: `16` bytes.

| Offset | Tamanho | Tipo | Campo |
| --- | --- | --- | --- |
| `0` | `1` | `uint8` | `success` |
| `1` | `3` | reservado | zeros |
| `4` | `4` | `int32` | `sdk_error` |
| `8` | `4` | `uint32` | `light_frames` |
| `12` | `4` | `uint32` | `dark_frames` |

O lado Matlab usa esse pacote apenas para atualizar o título da figura.

## Comportamento do `SpecSensorRgbStreamServer.m`

### Recepção

O servidor Matlab:

- abre `tcpserver(host, port)`;
- acumula bytes em `RxBuffer`;
- espera pelo menos `12` bytes para parsear o header;
- aguarda `HeaderBytes + payload_length`;
- despacha a mensagem por `message_type`;
- sempre responde com ACK de sucesso ou falha.

### `handleBegin`

Ao receber `Begin`, o Matlab:

- valida tamanho mínimo do payload;
- lê geometria e valores alvo RGB;
- valida se o bloco de comprimentos de onda tem o tamanho esperado;
- escolhe a classe do payload por `byte_depth`;
- resolve os índices das bandas RGB pela banda mais próxima de cada alvo;
- aloca:
  - `LightCubeRaw(image_width, band_count, expected_light_frames)`
  - `DarkCubeRaw(image_width, band_count, expected_dark_frames)`
  - `RgbPreviewRaw(expected_light_frames, image_width, 3)`

Mapeamento atual de `byte_depth`:

| `byte_depth` | classe Matlab |
| --- | --- |
| `1` | `uint8` |
| `2` | `uint16` |
| `4` | `uint32` |

### `handleChunk`

Ao receber um chunk:

- valida que existe um job ativo;
- valida que `payload_length` é múltiplo de `frame_size_bytes`;
- faz `typecast` para a classe definida pelo `byte_depth`;
- faz `reshape(values, [image_width, band_count, frameCount])`.

Para `DarkBlock`:

- grava no `DarkCubeRaw`;
- atualiza apenas o cursor dark;
- não renderiza preview.

Para `LightBlock`:

- grava no `LightCubeRaw`;
- extrai as três bandas RGB resolvidas;
- grava em `RgbPreviewRaw`;
- chama `renderCurrentImage()`.

### Renderização do preview

O preview atual:

- usa apenas a fase `LIGHT`;
- normaliza cada canal independentemente entre o menor e maior valor vistos no frame acumulado;
- publica uma imagem RGB `double` entre `0` e `1`;
- atualiza a figura com `drawnow limitrate nocallbacks`.

### `handleEnd`

Ao receber `End`, o Matlab:

- valida o tamanho do payload;
- lê `success`, `sdk_error`, `lightFrames` e `darkFrames`;
- renderiza novamente a imagem atual;
- atualiza o título da figura.

O servidor Matlab atual não persiste arquivos em disco.

## Relação com a `SharedWorkQueue`

O stream Matlab consome exatamente os mesmos itens que o `SaveCore`.

Consequências diretas:

- a ordem lógica observada pelo Matlab é a mesma do save;
- um ACK de stream atrasado impede a liberação do slot da fila;
- com stream habilitado, a capacidade real da fila é `3`, o que torna o Matlab um limitador importante de throughput.

## Limitações atuais

- O stream só funciona em Windows.
- O protocolo não transmite `sample_name`, `job_id` nem timestamps.
- O protocolo não transmite `first_frame_number` e `last_frame_number`.
- Os `double`s do payload `Begin` dependem do layout do host atual.
- O servidor Matlab aloca cubos completos para `expected_light_frames` e `expected_dark_frames`; isso aumenta consumo de memória para capturas longas.
- Um problema no Matlab é tratado como falha fatal do workflow, não como stream degradado.
