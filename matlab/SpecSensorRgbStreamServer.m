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
                return;
            end

            obj.Server = tcpserver(obj.Host, obj.Port, "ConnectionChangedFcn", ...
                @(src, evt)obj.handleConnectionChanged(src, evt));
            configureCallback(obj.Server, "byte", obj.HeaderBytes, @(src, evt)obj.handleData(src, evt));
            fprintf("[matlab-stream] Listening on %s:%d\n", obj.Host, obj.Port);
        end

        function delete(obj)
            obj.stop();
        end

        function stop(obj)
            if ~isempty(obj.Server) && isvalid(obj.Server)
                configureCallback(obj.Server, "off");
                server = obj.Server;
                obj.Server = [];
                clear server;
            end
        end

        function handleConnectionChanged(obj, src, ~)
            if strcmpi(src.Connected, "on")
                fprintf("[matlab-stream] Client connected\n");
                obj.resetJobState();
            else
                fprintf("[matlab-stream] Client disconnected\n");
                obj.resetJobState();
            end
        end

        function handleData(obj, src, ~)
            if src.NumBytesAvailable <= 0
                return;
            end

            incoming = read(src, src.NumBytesAvailable, "uint8");
            obj.RxBuffer = [obj.RxBuffer; incoming(:)]; %#ok<AGROW>

            while true
                if numel(obj.RxBuffer) < obj.HeaderBytes
                    return;
                end

                header = obj.parseHeader(obj.RxBuffer(1:obj.HeaderBytes));
                totalBytes = obj.HeaderBytes + double(header.payload_length);
                if numel(obj.RxBuffer) < totalBytes
                    return;
                end

                payloadBytes = obj.RxBuffer(obj.HeaderBytes + 1 : totalBytes);
                obj.RxBuffer = obj.RxBuffer(totalBytes + 1:end);

                ackSuccess = true;
                try
                    obj.dispatchMessage(header, payloadBytes);
                catch err
                    ackSuccess = false;
                    fprintf("[matlab-stream] Error processing message_type=%d: %s\n", ...
                        header.message_type, err.message);
                end
                obj.writeAck(src, ackSuccess);
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

            obj.LightCubeRaw = zeros(imageWidth, bandCount, expectedLightFrames, obj.PayloadClass);
            obj.DarkCubeRaw = zeros(imageWidth, bandCount, expectedDarkFrames, obj.PayloadClass);
            obj.RgbPreviewRaw = zeros(expectedLightFrames, imageWidth, 3, obj.PayloadClass);

            obj.ensureFigure(imageWidth);
            obj.updateFigureTitle(sprintf("LIGHT aguardando | RGB alvo=[%d %d %d]nm | bandas=[%.2f %.2f %.2f]nm", ...
                obj.RgbTargetWavelengthNm(1), obj.RgbTargetWavelengthNm(2), obj.RgbTargetWavelengthNm(3), ...
                obj.ResolvedRgbBandWavelengthNm(1), obj.ResolvedRgbBandWavelengthNm(2), ...
                obj.ResolvedRgbBandWavelengthNm(3)));
        end

        function handleChunk(obj, payloadBytes, isDark)
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

            if isDark
                startIndex = obj.DarkFrameCursor + 1;
                endIndex = obj.DarkFrameCursor + frameCount;
                obj.DarkCubeRaw(:, :, startIndex:endIndex) = block;
                obj.DarkFrameCursor = endIndex;
                obj.updateFigureTitle(sprintf("DARK recebidos=%d", obj.DarkFrameCursor));
                return;
            end

            startIndex = obj.LightFrameCursor + 1;
            endIndex = obj.LightFrameCursor + frameCount;
            obj.LightCubeRaw(:, :, startIndex:endIndex) = block;

            obj.RgbPreviewRaw(startIndex:endIndex, :, 1) = permute(block(:, obj.ResolvedRgbBandIndices(1), :), [3 1 2]);
            obj.RgbPreviewRaw(startIndex:endIndex, :, 2) = permute(block(:, obj.ResolvedRgbBandIndices(2), :), [3 1 2]);
            obj.RgbPreviewRaw(startIndex:endIndex, :, 3) = permute(block(:, obj.ResolvedRgbBandIndices(3), :), [3 1 2]);
            obj.LightFrameCursor = endIndex;

            obj.renderCurrentImage();
            obj.updateFigureTitle(sprintf("LIGHT linhas=%d/%d | RGB=[%.2f %.2f %.2f]nm", ...
                obj.LightFrameCursor, obj.Job.expected_light_frames, ...
                obj.ResolvedRgbBandWavelengthNm(1), obj.ResolvedRgbBandWavelengthNm(2), ...
                obj.ResolvedRgbBandWavelengthNm(3)));
        end

        function handleEnd(obj, payloadBytes)
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
                return;
            end

            obj.FigureHandle = figure("Name", "SpecSensor RGB Stream", "NumberTitle", "off");
            obj.AxesHandle = axes(obj.FigureHandle);
            initialImage = zeros(1, max(1, imageWidth), 3, "double");
            obj.ImageHandle = image(obj.AxesHandle, initialImage);
            axis(obj.AxesHandle, "image");
            obj.AxesHandle.YDir = "normal";
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
                return;
            end
            title(obj.AxesHandle, char(textValue), "Interpreter", "none");
        end

        function writeAck(obj, src, success)
            ackBytes = obj.buildAck(success);
            write(src, ackBytes, "uint8");
        end

        function ackBytes = buildAck(obj, success)
            statusValue = uint16(~success);
            ackBytes = zeros(obj.AckBytes, 1, "uint8");
            ackBytes(1:4) = uint8("SSFA");
            ackBytes(5:6) = typecast(uint16(1), "uint8");
            ackBytes(7:8) = typecast(statusValue, "uint8");
        end

        function resetJobState(obj)
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
    end
end
