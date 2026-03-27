# Agent Prompt Base: Capture Stability

Use esta base curta para qualquer agente que vá mexer em captura, inicialização da câmera, build ou empacotamento.

## Prompt curto

```text
Este projeto já teve histórico de travamento durante a fase LIGHT ou na transição LIGHT -> DARK. A causa mais provável foi multifatorial: resolução/runtime de DLL pouco determinístico no projeto antigo, somado a uma transição de fase menos defensiva.

Regras obrigatórias:

1. Preserve resolução determinística das DLLs da SDK.
- Não remover ConfigureSpecSensorDllSearchPath().
- Não voltar a copiar DLLs cegamente de bins genéricos/compartilhados.

2. Preserve ownership da SDK.
- CaptureCore deve continuar sendo o dono principal do handle e do fluxo da câmera.
- Não espalhar chamadas SI_* por SaveCore, PipeCore, callbacks ou threads auxiliares.

3. Preserve a transição de captura.
- Não remover Acquisition.RingBuffer.Sync antes do LIGHT.
- Não remover a sequência CloseShutter -> Acquisition.Stop -> espera curta -> Acquisition.Start antes do DARK.
- Não simplificar essa troca sem validação em hardware.

4. Preserve a ordem de configuração.
- Trigger.Mode = Internal
- Binning
- ExposureTime.Auto = false
- ExposureTime
- FrameRate

5. Trate mudanças nestas áreas como alto risco:
- build/runtime/DLL;
- thread model;
- fluxo LIGHT/DARK;
- fallbacks automáticos de features/canais;
- ordem de configuração da câmera.

6. Prefira simplicidade explícita.
- Se houver dúvida entre fluxo curto e explícito versus fallback complexo, prefira o fluxo curto e explícito.
- Não reintroduzir heurísticas que aumentem a superfície de estado interno da SDK.

7. Ao alterar captura ou build:
- manter ou atualizar os testes que validam RingBuffer.Sync, restart antes do DARK e logs de fase;
- assumir que estabilidade em hardware vale mais que conveniência estrutural.
```

## Versão ainda mais curta

```text
Histórico crítico: travamento em LIGHT e na transição LIGHT -> DARK.

Não quebrar:
- DLL da SDK com resolução determinística;
- SDK concentrada no CaptureCore;
- RingBuffer.Sync antes do LIGHT;
- Stop + pequena espera + Start antes do DARK;
- ordem Trigger -> Binning -> Exposure Auto Off -> Exposure -> FrameRate.

Tratar como alto risco:
- mudanças de build/runtime;
- novas threads falando com a SDK;
- fallback automático extra;
- simplificação do fluxo LIGHT/DARK sem teste em hardware.
```

## Referência longa

Se precisar do racional técnico completo, usar:

- `docs/capture_stability_lessons.md`
