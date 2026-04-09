# Documentação da Captura e Integração Matlab

Esta pasta concentra a documentação técnica do pipeline de captura atual.

O objetivo é manter em um único lugar lógico:

- a arquitetura de execução;
- a interação do `CaptureCore` com a `ISpecSensorApi`;
- o fluxo de persistência em disco;
- o fluxo TCP para Matlab;ku preciso que você elabore a documentação referente à implementação da api que vc construiu, bem como os pontos principais de interação do CaptureCore com a api. Ademais, preciso que vc elabore também a documentação da parte de salvamento e transmissão dos dados via TCP para o matlab, considere explicar a arquitetura geral, filas, pacotes, principais valores. Decida também a melhor ferramenta de documentação, não queria colocar no código, pois vai ficar muito poluído. QUeria concentrar a documentação toda em um só lugar
- a referência rápida de classes, métodos, contratos e valores padrão.

## Visão curta do sistema

O executável sobe a câmera, recebe comandos `CAPTURE <sample_name>` via named pipe e executa um workflow serializado por amostra.

O desenho atual separa responsabilidades assim:

- `CaptureCore` é o único dono da relação com a SDK da câmera.
- `main` orquestra workers, fila compartilhada, estado de runtime e UI.
- `SaveCore` consome eventos de trabalho para persistir `RAW`, `HDR`, logs de drop e `PNG`.
- `FrameStreamCore` consome os mesmos eventos para transmitir os dados ao Matlab via TCP.
- `SharedWorkQueue` faz o fan-out dos mesmos `WorkItem`s para `SaveCore` e `FrameStreamCore`.

## Ordem sugerida de leitura

1. [`architecture.md`](architecture.md)
   Explica o fluxo principal do processo, ownership da SDK, filas e propagação de falhas.
2. [`matlab_stream.md`](matlab_stream.md)
   Isola tudo que é específico do stream TCP e do consumidor Matlab.
3. [`api_reference.md`](api_reference.md)
   Serve como referência de classes, métodos e tipos de contrato.
4. [`defaults_and_constants.md`](defaults_and_constants.md)
   Lista defaults, constantes operacionais e parâmetros que hoje não alteram o runtime real.

## Quando consultar cada arquivo

- Consulte [`architecture.md`](architecture.md) antes de mexer em orquestração, captura, filas ou lifecycle.
- Consulte [`matlab_stream.md`](matlab_stream.md) antes de mexer no protocolo binário, no `FrameStreamCore` ou no script Matlab.
- Consulte [`api_reference.md`](api_reference.md) quando precisar lembrar rapidamente quem chama quem, em qual thread cada classe atua e quais efeitos colaterais um método produz.
- Consulte [`defaults_and_constants.md`](defaults_and_constants.md) quando precisar revisar parâmetros, tamanhos, timeouts, valores default e lacunas conhecidas.

## Limite desta documentação

Esta pasta documenta o comportamento atual do código. Quando houver diferença entre o texto e a implementação, o código e os testes continuam sendo a fonte final de verdade.

Documentos históricos fora desta pasta continuam úteis para contexto de estabilidade, em especial:

- `docs/capture_stability_lessons.md`
- `docs/agent_capture_stability_prompt.md`
