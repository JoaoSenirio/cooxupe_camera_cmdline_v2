classdef SpecSensorRgbStreamServer < handle
    properties
        Host (1, :) char = '127.0.0.1'
        Port (1, 1) double = 55001
        Server
        RxBuffer uint8 = uint8([])
        Job struct = struct()
        HeaderBytes (1, 1) double = 12
        AckBytes (1, 1) double = 8
        LightFrameCursor (1, 1) double = 0
        DarkFrameCursor (1, 1) double = 0
        PayloadClass (1, :) char = 'uint16'
        VerboseLogging (1, 1) logical = true
        MaxLoggedBytes (1, 1) double = 32
        MaxCubeBytes (1, 1) double = 512 * 1024 * 1024
        StoreLightCube (1, 1) logical = true
        StoreDarkCube (1, 1) logical = true
        LightCubeRaw
        DarkCubeRaw
        RgbPreviewRaw
        RgbTargetWavelengthNm (1, 3) double = [0 0 0]
        ResolvedRgbBandIndices (1, 3) double = [1 1 1]
        ResolvedRgbBandWavelengthNm (1, 3) double = [0 0 0]
        FigureHandle
        AxesHandle
        ImageHandle
    end

    methods
        function obj = SpecSensorRgbStreamServer(host, port)
            if nargin >= 1 && ~isempty(host)
                obj.Host = char(host);
            end
            if nargin >= 2 && ~isempty(port)
                obj.Port = double(port);
            end
        end

        function start(obj)
            if ~isempty(obj.Server) && isvalid(obj.Server)
                obj.logf("start skipped: server already valid on %s:%d", obj.Host, obj.Port);
                return;
            end

            obj.logf("start requested host=%s port=%d", obj.Host, obj.Port);
            obj.Server = tcpserver(obj.Host, obj.Port, "ConnectionChangedFcn", ...
                @(src, evt)obj.handleConnectionChanged(src, evt));
            configureCallback(obj.Server, "byte", obj.HeaderBytes, @(src, evt)obj.handleData(src, evt));
            obj.logf("Listening on %s:%d header_bytes=%d ack_bytes=%d", ...
                obj.Host, obj.Port, obj.HeaderBytes, obj.AckBytes);
        end

        function delete(obj)
            obj.stop();
        end

        function stop(obj)
            if ~isempty(obj.Server) && isvalid(obj.Server)
                obj.logf("stop requested");
                configureCallback(obj.Server, "off");
                server = obj.Server;
                obj.Server = [];
                clear server;
                obj.logf("server stopped");
            end
        end

        function handleConnectionChanged(obj, src, ~)
            if strcmpi(src.Connected, "on")
                obj.logf("Client connected num_bytes_available=%d", double(src.NumBytesAvailable));
                obj.resetJobState();
            else
                obj.logf("Client disconnected num_bytes_available=%d", double(src.NumBytesAvailable));
                obj.resetJobState();
            end
        end

        function handleData(obj, src, ~)
            if src.NumBytesAvailable <= 0
                obj.logf("handleData invoked with no available bytes");
                return;
            end

            obj.logf("handleData enter available=%d rx_buffer_before=%d", ...
                double(src.NumBytesAvailable), numel(obj.RxBuffer));
            incoming = read(src, src.NumBytesAvailable, "uint8");
            obj.RxBuffer = [obj.RxBuffer; incoming(:)]; %#ok<AGROW>
            obj.logf("handleData read bytes=%d rx_buffer_after=%d first_bytes=%s", ...
                numel(incoming), numel(obj.RxBuffer), obj.bytesToHex(incoming));

            while true
                if numel(obj.RxBuffer) < obj.HeaderBytes
                    obj.logf("handleData waiting header rx_buffer=%d header_bytes=%d", ...
                        numel(obj.RxBuffer), obj.HeaderBytes);
                    return;
                end

                parseStarted = tic;
                try
                    header = obj.parseHeader(obj.RxBuffer(1:obj.HeaderBytes));
                catch err
                    obj.logf("parseHeader failed rx_buffer=%d header_hex=%s error=%s", ...
                        numel(obj.RxBuffer), obj.bytesToHex(obj.RxBuffer(1:obj.HeaderBytes)), ...
                        obj.formatError(err));
                    rethrow(err);
                end
                parseElapsedMs = toc(parseStarted) * 1000.0;
                totalBytes = obj.HeaderBytes + double(header.payload_length);
                obj.logf("parsed header type=%d(%s) version=%d payload_length=%d total_bytes=%d rx_buffer=%d parse_ms=%.3f header_hex=%s", ...
                    header.message_type, obj.messageTypeName(header.message_type), ...
                    header.version, header.payload_length, totalBytes, numel(obj.RxBuffer), ...
                    parseElapsedMs, obj.bytesToHex(obj.RxBuffer(1:obj.HeaderBytes)));
                if numel(obj.RxBuffer) < totalBytes
                    obj.logf("handleData waiting payload type=%d(%s) need=%d have=%d", ...
                        header.message_type, obj.messageTypeName(header.message_type), ...
                        totalBytes, numel(obj.RxBuffer));
                    return;
                end

                payloadBytes = obj.RxBuffer(obj.HeaderBytes + 1 : totalBytes);
                obj.RxBuffer = obj.RxBuffer(totalBytes + 1:end);
                obj.logf("dispatch start type=%d(%s) payload_bytes=%d payload_head=%s rx_buffer_remaining=%d", ...
                    header.message_type, obj.messageTypeName(header.message_type), ...
                    numel(payloadBytes), obj.bytesToHex(payloadBytes), numel(obj.RxBuffer));

                ackSuccess = true;
                dispatchElapsedMs = 0.0;
                try
                    dispatchStarted = tic;
                    obj.dispatchMessage(header, payloadBytes);
                    dispatchElapsedMs = toc(dispatchStarted) * 1000.0;
                    obj.logf("dispatch ok type=%d(%s) elapsed_ms=%.3f", ...
                        header.message_type, obj.messageTypeName(header.message_type), dispatchElapsedMs);
                catch err
                    ackSuccess = false;
                    dispatchElapsedMs = toc(dispatchStarted) * 1000.0;
                    obj.logf("Error processing message_type=%d(%s) elapsed_ms=%.3f error=%s", ...
                        header.message_type, obj.messageTypeName(header.message_type), ...
                        dispatchElapsedMs, obj.formatError(err));
                end
                try
                    obj.writeAck(src, ackSuccess, header.message_type);
                catch err
                    obj.logf("writeAck failed type=%d(%s) error=%s", ...
                        header.message_type, obj.messageTypeName(header.message_type), ...
                        obj.formatError(err));
                    rethrow(err);
                end
            end
        end

        function header = parseHeader(obj, bytes)
            if numel(bytes) < obj.HeaderBytes
                error("SpecSensorRgbStreamServer:ShortHeader", "Header incompleto");
            end

            magic = char(bytes(1:4)).';
            if ~strcmp(magic, "SSFR")
                error("SpecSensorRgbStreamServer:BadMagic", "Magic invalido: %s", magic);
            end

            header = struct();
            header.version = double(typecast(uint8(bytes(5:6)), "uint16"));
            header.message_type = double(typecast(uint8(bytes(7:8)), "uint16"));
            header.payload_length = double(typecast(uint8(bytes(9:12)), "uint32"));
        end

        function dispatchMessage(obj, header, payloadBytes)
            switch header.message_type
                case 1
                    obj.handleBegin(payloadBytes);
                case 2
                    obj.handleChunk(payloadBytes, false);
                case 3
                    obj.handleChunk(payloadBytes, true);
                case 4
                    obj.handleEnd(payloadBytes);
                otherwise
                    error("SpecSensorRgbStreamServer:UnknownMessageType", ...
                        "message_type desconhecido: %d", header.message_type);
            end
        end

        function handleBegin(obj, payloadBytes)
            beginStarted = tic;
            if numel(payloadBytes) < 36
                error("SpecSensorRgbStreamServer:ShortBegin", "Payload Begin incompleto");
            end

            imageWidth = double(typecast(uint8(payloadBytes(1:4)), "uint32"));
            bandCount = double(typecast(uint8(payloadBytes(5:8)), "uint32"));
            byteDepth = double(typecast(uint8(payloadBytes(9:12)), "uint32"));
            frameSizeBytes = double(typecast(uint8(payloadBytes(13:16)), "uint32"));
            expectedLightFrames = double(typecast(uint8(payloadBytes(17:20)), "uint32"));
            expectedDarkFrames = double(typecast(uint8(payloadBytes(21:24)), "uint32"));
            rgbTargets = double(typecast(uint8(payloadBytes(25:36)), "uint32"));

            wavelengthsBytes = payloadBytes(37:end);
            expectedWavelengthBytes = bandCount * 8;
            if numel(wavelengthsBytes) ~= expectedWavelengthBytes
                error("SpecSensorRgbStreamServer:BadWavelengthBlock", ...
                    "Bloco de comprimentos de onda invalido. Esperado=%d Recebido=%d", ...
                    expectedWavelengthBytes, numel(wavelengthsBytes));
            end

            wavelengthsNm = double(typecast(uint8(wavelengthsBytes), "double"));
            if numel(wavelengthsNm) ~= bandCount
                error("SpecSensorRgbStreamServer:BadBandCount", ...
                    "Quantidade de bandas inconsistente");
            end
            if bandCount < 3
                error("SpecSensorRgbStreamServer:TooFewBands", ...
                    "Sao necessarias pelo menos 3 bandas para o preview RGB");
            end

            obj.PayloadClass = obj.classForByteDepth(byteDepth);
            obj.Job = struct( ...
                "image_width", imageWidth, ...
                "band_count", bandCount, ...
                "byte_depth", byteDepth, ...
                "frame_size_bytes", frameSizeBytes, ...
                "expected_light_frames", expectedLightFrames, ...
                "expected_dark_frames", expectedDarkFrames, ...
                "wavelengths_nm", wavelengthsNm);
            obj.LightFrameCursor = 0;
            obj.DarkFrameCursor = 0;
            obj.RgbTargetWavelengthNm = reshape(rgbTargets, 1, 3);
            obj.ResolvedRgbBandIndices = obj.resolveRgbBandIndices(wavelengthsNm, obj.RgbTargetWavelengthNm);
            obj.ResolvedRgbBandWavelengthNm = wavelengthsNm(obj.ResolvedRgbBandIndices);
            obj.logf("handleBegin image_width=%d band_count=%d byte_depth=%d frame_size_bytes=%d expected_light_frames=%d expected_dark_frames=%d rgb_targets=[%d %d %d] resolved_rgb_indices=[%d %d %d]", ...
                imageWidth, bandCount, byteDepth, frameSizeBytes, expectedLightFrames, expectedDarkFrames, ...
                obj.RgbTargetWavelengthNm(1), obj.RgbTargetWavelengthNm(2), obj.RgbTargetWavelengthNm(3), ...
                obj.ResolvedRgbBandIndices(1), obj.ResolvedRgbBandIndices(2), obj.ResolvedRgbBandIndices(3));

            lightCubeBytes = frameSizeBytes * expectedLightFrames;
            darkCubeBytes = frameSizeBytes * expectedDarkFrames;
            obj.StoreLightCube = lightCubeBytes > 0 && lightCubeBytes <= obj.MaxCubeBytes;
            obj.StoreDarkCube = darkCubeBytes > 0 && darkCubeBytes <= obj.MaxCubeBytes;

            if obj.StoreLightCube
                obj.LightCubeRaw = zeros(imageWidth, bandCount, expectedLightFrames, obj.PayloadClass);
            else
                obj.LightCubeRaw = [];
                obj.logf("Skipping LightCubeRaw preallocation: %.2f GiB exceeds %.2f GiB limit", ...
                    lightCubeBytes / (1024 ^ 3), obj.MaxCubeBytes / (1024 ^ 3));
            end

            if obj.StoreDarkCube
                obj.DarkCubeRaw = zeros(imageWidth, bandCount, expectedDarkFrames, obj.PayloadClass);
            else
                obj.DarkCubeRaw = [];
                obj.logf("Skipping DarkCubeRaw preallocation: %.2f GiB exceeds %.2f GiB limit", ...
                    darkCubeBytes / (1024 ^ 3), obj.MaxCubeBytes / (1024 ^ 3));
            end

            obj.RgbPreviewRaw = zeros(0, imageWidth, 3, obj.PayloadClass);

            obj.ensureFigure(imageWidth);
            obj.updateFigureTitle(sprintf("LIGHT aguardando | RGB alvo=[%d %d %d]nm | bandas=[%.2f %.2f %.2f]nm", ...
                obj.RgbTargetWavelengthNm(1), obj.RgbTargetWavelengthNm(2), obj.RgbTargetWavelengthNm(3), ...
                obj.ResolvedRgbBandWavelengthNm(1), obj.ResolvedRgbBandWavelengthNm(2), ...
                obj.ResolvedRgbBandWavelengthNm(3)));
            obj.logf("handleBegin finished elapsed_ms=%.3f preview_rows=%d light_store=%d dark_store=%d", ...
                toc(beginStarted) * 1000.0, size(obj.RgbPreviewRaw, 1), obj.StoreLightCube, obj.StoreDarkCube);
        end

        function handleChunk(obj, payloadBytes, isDark)
            chunkStarted = tic;
            if isempty(fieldnames(obj.Job))
                error("SpecSensorRgbStreamServer:NoJob", "Chunk recebido sem Begin");
            end

            frameSizeBytes = obj.Job.frame_size_bytes;
            if frameSizeBytes <= 0 || mod(numel(payloadBytes), frameSizeBytes) ~= 0
                error("SpecSensorRgbStreamServer:BadChunkSize", ...
                    "Chunk com payload invalido. bytes=%d frame_size=%d", ...
                    numel(payloadBytes), frameSizeBytes);
            end

            frameCount = numel(payloadBytes) / frameSizeBytes;
            values = typecast(uint8(payloadBytes), obj.PayloadClass);
            block = reshape(values, [obj.Job.image_width, obj.Job.band_count, frameCount]);
            obj.logf("handleChunk kind=%s payload_bytes=%d frame_count=%d frame_size_bytes=%d cursor_light=%d cursor_dark=%d", ...
                obj.chunkKindName(isDark), numel(payloadBytes), frameCount, frameSizeBytes, ...
                obj.LightFrameCursor, obj.DarkFrameCursor);

            if isDark
                startIndex = obj.DarkFrameCursor + 1;
                endIndex = obj.DarkFrameCursor + frameCount;
                if obj.StoreDarkCube
                    obj.DarkCubeRaw(:, :, startIndex:endIndex) = block;
                end
                obj.DarkFrameCursor = endIndex;
                obj.updateFigureTitle(sprintf("DARK recebidos=%d", obj.DarkFrameCursor));
                obj.logf("handleChunk dark stored=%d start_index=%d end_index=%d elapsed_ms=%.3f", ...
                    obj.StoreDarkCube, startIndex, endIndex, toc(chunkStarted) * 1000.0);
                return;
            end

            startIndex = obj.LightFrameCursor + 1;
            endIndex = obj.LightFrameCursor + frameCount;
            if obj.StoreLightCube
                obj.LightCubeRaw(:, :, startIndex:endIndex) = block;
            end

            previewBlock = zeros(frameCount, obj.Job.image_width, 3, obj.PayloadClass);
            previewBlock(:, :, 1) = permute(block(:, obj.ResolvedRgbBandIndices(1), :), [3 1 2]);
            previewBlock(:, :, 2) = permute(block(:, obj.ResolvedRgbBandIndices(2), :), [3 1 2]);
            previewBlock(:, :, 3) = permute(block(:, obj.ResolvedRgbBandIndices(3), :), [3 1 2]);
            if isempty(obj.RgbPreviewRaw)
                obj.RgbPreviewRaw = previewBlock;
            else
                obj.RgbPreviewRaw = cat(1, obj.RgbPreviewRaw, previewBlock);
            end
            obj.LightFrameCursor = endIndex;

            renderStarted = tic;
            obj.renderCurrentImage();
            renderElapsedMs = toc(renderStarted) * 1000.0;
            obj.updateFigureTitle(sprintf("LIGHT linhas=%d/%d | RGB=[%.2f %.2f %.2f]nm", ...
                obj.LightFrameCursor, obj.Job.expected_light_frames, ...
                obj.ResolvedRgbBandWavelengthNm(1), obj.ResolvedRgbBandWavelengthNm(2), ...
                obj.ResolvedRgbBandWavelengthNm(3)));
            obj.logf("handleChunk light stored=%d start_index=%d end_index=%d preview_rows=%d render_ms=%.3f total_ms=%.3f", ...
                obj.StoreLightCube, startIndex, endIndex, size(obj.RgbPreviewRaw, 1), ...
                renderElapsedMs, toc(chunkStarted) * 1000.0);
        end

        function handleEnd(obj, payloadBytes)
            endStarted = tic;
            if numel(payloadBytes) ~= 16
                error("SpecSensorRgbStreamServer:BadEnd", "Payload End invalido");
            end

            successValue = payloadBytes(1) ~= 0;
            sdkError = double(typecast(uint8(payloadBytes(5:8)), "int32"));
            lightFrames = double(typecast(uint8(payloadBytes(9:12)), "uint32"));
            darkFrames = double(typecast(uint8(payloadBytes(13:16)), "uint32"));

            obj.renderCurrentImage();
            obj.updateFigureTitle(sprintf("fim success=%d sdk=%d light=%d dark=%d", ...
                successValue, sdkError, lightFrames, darkFrames));
            obj.logf("handleEnd success=%d sdk_error=%d light_frames=%d dark_frames=%d elapsed_ms=%.3f", ...
                successValue, sdkError, lightFrames, darkFrames, toc(endStarted) * 1000.0);
        end

        function indices = resolveRgbBandIndices(~, wavelengthsNm, rgbTargets)
            indices = zeros(1, 3);
            for channel = 1:3
                [~, idx] = min(abs(wavelengthsNm - rgbTargets(channel)));
                indices(channel) = idx;
            end
        end

        function ensureFigure(obj, imageWidth)
            if ~isempty(obj.FigureHandle) && isvalid(obj.FigureHandle)
                obj.logf("ensureFigure reused existing figure image_width=%d", imageWidth);
                return;
            end

            figureStarted = tic;
            obj.FigureHandle = figure("Name", "SpecSensor RGB Stream", "NumberTitle", "off");
            obj.AxesHandle = axes(obj.FigureHandle);
            initialImage = zeros(1, max(1, imageWidth), 3, "double");
            obj.ImageHandle = image(obj.AxesHandle, initialImage);
            axis(obj.AxesHandle, "image");
            obj.AxesHandle.YDir = "normal";
            obj.logf("ensureFigure created image_width=%d elapsed_ms=%.3f", ...
                imageWidth, toc(figureStarted) * 1000.0);
        end

        function renderCurrentImage(obj)
            if obj.LightFrameCursor <= 0
                return;
            end

            raw = double(obj.RgbPreviewRaw(1:obj.LightFrameCursor, :, :));
            displayImage = zeros(size(raw, 1), size(raw, 2), 3, "double");
            for channel = 1:3
                plane = raw(:, :, channel);
                low = min(plane, [], "all");
                high = max(plane, [], "all");
                if high > low
                    plane = (plane - low) ./ (high - low);
                else
                    plane(:, :) = 0;
                end
                displayImage(:, :, channel) = plane;
            end

            obj.ensureFigure(size(raw, 2));
            obj.ImageHandle.CData = displayImage;
            axis(obj.AxesHandle, "image");
            drawnow limitrate nocallbacks;
        end

        function updateFigureTitle(obj, textValue)
            if isempty(obj.AxesHandle) || ~isvalid(obj.AxesHandle)
                obj.logf("updateFigureTitle skipped: axes not valid text=%s", char(textValue));
                return;
            end
            title(obj.AxesHandle, char(textValue), "Interpreter", "none");
        end

        function writeAck(obj, src, success, messageType)
            ackBytes = obj.buildAck(success);
            ackStarted = tic;
            obj.logf("writeAck start type=%d(%s) status=%d bytes=%s", ...
                messageType, obj.messageTypeName(messageType), uint16(~success), ...
                obj.bytesToHex(ackBytes));
            write(src, ackBytes, "uint8");
            obj.logf("writeAck done type=%d(%s) status=%d elapsed_ms=%.3f", ...
                messageType, obj.messageTypeName(messageType), uint16(~success), ...
                toc(ackStarted) * 1000.0);
        end

        function ackBytes = buildAck(obj, success)
            statusValue = uint16(~success);
            ackBytes = zeros(obj.AckBytes, 1, "uint8");
            % Use char -> uint8 for compatibility with Matlab versions that
            % do not support direct string -> uint8 conversion.
            ackBytes(1:4) = uint8('SSFA').';
            ackBytes(5:6) = reshape(typecast(uint16(1), "uint8"), [], 1);
            ackBytes(7:8) = reshape(typecast(statusValue, "uint8"), [], 1);
        end

        function resetJobState(obj)
            obj.logf("resetJobState begin rx_buffer=%d light_cursor=%d dark_cursor=%d", ...
                numel(obj.RxBuffer), obj.LightFrameCursor, obj.DarkFrameCursor);
            obj.RxBuffer = uint8([]);
            obj.Job = struct();
            obj.LightFrameCursor = 0;
            obj.DarkFrameCursor = 0;
            obj.LightCubeRaw = [];
            obj.DarkCubeRaw = [];
            obj.RgbPreviewRaw = [];
            obj.RgbTargetWavelengthNm = [0 0 0];
            obj.ResolvedRgbBandIndices = [1 1 1];
            obj.ResolvedRgbBandWavelengthNm = [0 0 0];
            obj.StoreLightCube = true;
            obj.StoreDarkCube = true;
            obj.logf("resetJobState done");
        end

        function className = classForByteDepth(~, byteDepth)
            switch byteDepth
                case 1
                    className = 'uint8';
                case 2
                    className = 'uint16';
                case 4
                    className = 'uint32';
                otherwise
                    error("SpecSensorRgbStreamServer:UnsupportedByteDepth", ...
                        "byte_depth nao suportado: %d", byteDepth);
            end
        end

        function name = messageTypeName(~, messageType)
            switch messageType
                case 1
                    name = 'Begin';
                case 2
                    name = 'LightBlock';
                case 3
                    name = 'DarkBlock';
                case 4
                    name = 'End';
                otherwise
                    name = sprintf('Unknown(%d)', messageType);
            end
        end

        function name = chunkKindName(~, isDark)
            if isDark
                name = 'dark';
            else
                name = 'light';
            end
        end

        function text = bytesToHex(obj, bytes)
            if isempty(bytes)
                text = '<empty>';
                return;
            end

            flatBytes = uint8(bytes(:));
            count = min(numel(flatBytes), obj.MaxLoggedBytes);
            parts = cell(1, count);
            for i = 1:count
                parts{i} = sprintf('%02X', flatBytes(i));
            end
            text = strjoin(parts, ' ');
            if numel(flatBytes) > count
                text = sprintf('%s ... (%d bytes total)', text, numel(flatBytes));
            end
        end

        function text = formatError(~, err)
            text = char(err.message);
            if ~isempty(err.stack)
                top = err.stack(1);
                text = sprintf('%s | top=%s:%d', text, top.name, top.line);
            end
        end

        function logf(obj, varargin)
            if ~obj.VerboseLogging
                return;
            end

            timestamp = datestr(now, 'yyyy-mm-dd HH:MM:SS.FFF');
            fprintf('[matlab-stream] [%s] %s\n', timestamp, sprintf(varargin{:}));
        end
    end
end
