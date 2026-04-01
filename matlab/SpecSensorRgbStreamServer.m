classdef SpecSensorRgbStreamServer < handle
    properties
        Host (1, :) char = '127.0.0.1'
        Port (1, 1) double = 55001
        Server
        RxBuffer uint8 = uint8([])
        Job struct = struct()
        LineCursor (1, 1) double = 0
        RgbCubeRaw uint16 = uint16([])
        FigureHandle
        AxesHandle
        ImageHandle
        HeaderBytes (1, 1) double = 40
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
            configureCallback(obj.Server, "byte", 1, @(src, evt)obj.handleData(src, evt));
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
            else
                fprintf("[matlab-stream] Client disconnected\n");
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
                totalBytes = obj.HeaderBytes + double(header.metadata_length) + double(header.payload_length);
                if numel(obj.RxBuffer) < totalBytes
                    return;
                end

                metadataBytes = obj.RxBuffer(obj.HeaderBytes + 1 : obj.HeaderBytes + double(header.metadata_length));
                payloadOffset = obj.HeaderBytes + double(header.metadata_length) + 1;
                payloadEnd = totalBytes;
                payloadBytes = obj.RxBuffer(payloadOffset:payloadEnd);
                obj.RxBuffer = obj.RxBuffer(totalBytes + 1:end);

                metadataText = native2unicode(metadataBytes(:).', "UTF-8");
                metadata = jsondecode(metadataText);
                obj.dispatchMessage(header, metadata, payloadBytes);
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
            header.header_bytes = double(typecast(uint8(bytes(7:8)), "uint16"));
            header.message_type = double(typecast(uint8(bytes(9:12)), "uint32"));
            header.flags = double(typecast(uint8(bytes(13:16)), "uint32"));
            header.job_id = double(typecast(uint8(bytes(17:24)), "uint64"));
            header.sequence = double(typecast(uint8(bytes(25:32)), "uint64"));
            header.metadata_length = double(typecast(uint8(bytes(33:36)), "uint32"));
            header.payload_length = double(typecast(uint8(bytes(37:40)), "uint32"));
        end

        function dispatchMessage(obj, header, metadata, payloadBytes)
            switch header.message_type
                case 1
                    obj.handleJobBegin(header, metadata);
                case 2
                    obj.handleLightRgbBlock(header, metadata, payloadBytes);
                case 3
                    obj.handleJobEnd(header, metadata);
                otherwise
                    fprintf("[matlab-stream] Ignoring unknown message_type=%d\n", header.message_type);
            end
        end

        function handleJobBegin(obj, header, metadata)
            expectedLightFrames = max(0, double(metadata.expected_light_frames));
            imageWidth = max(0, double(metadata.image_width));

            obj.Job = struct( ...
                "job_id", header.job_id, ...
                "sample_name", string(metadata.sample_name), ...
                "camera_name", string(metadata.camera_name), ...
                "acquisition_name", string(metadata.acquisition_name), ...
                "final_png_path", string(metadata.final_png_path), ...
                "expected_light_frames", expectedLightFrames, ...
                "image_width", imageWidth);
            obj.LineCursor = 0;
            obj.RgbCubeRaw = zeros(expectedLightFrames, imageWidth, 3, "uint16");
            obj.ensureFigure(imageWidth);
            obj.updateFigureTitle(sprintf("%s | aguardando linhas", metadata.sample_name));
        end

        function handleLightRgbBlock(obj, ~, metadata, payloadBytes)
            if isempty(fieldnames(obj.Job))
                return;
            end

            lineCount = double(metadata.line_count);
            imageWidth = double(metadata.image_width);
            if lineCount <= 0 || imageWidth <= 0
                return;
            end

            values = typecast(uint8(payloadBytes), "uint16");
            expectedValues = lineCount * imageWidth * 3;
            if numel(values) ~= expectedValues
                error("SpecSensorRgbStreamServer:PayloadMismatch", ...
                    "Payload RGB inesperado. Esperado=%d Recebido=%d", expectedValues, numel(values));
            end

            block = permute(reshape(values, [3, imageWidth, lineCount]), [3, 2, 1]);
            obj.ensureCapacity(obj.LineCursor + lineCount, imageWidth);
            nextRange = obj.LineCursor + 1 : obj.LineCursor + lineCount;
            obj.RgbCubeRaw(nextRange, :, :) = block;
            obj.LineCursor = obj.LineCursor + lineCount;

            obj.renderCurrentImage();
            obj.updateFigureTitle(sprintf("%s | linhas=%d | frames=%d-%d", ...
                obj.Job.sample_name, obj.LineCursor, ...
                double(metadata.first_frame_number), double(metadata.last_frame_number)));
        end

        function handleJobEnd(obj, ~, metadata)
            if isempty(fieldnames(obj.Job))
                return;
            end

            statusText = "falhou";
            if isfield(metadata, "success") && metadata.success
                statusText = "concluido";
            end

            obj.renderCurrentImage();
            obj.updateFigureTitle(sprintf("%s | %s | png=%s", ...
                obj.Job.sample_name, statusText, string(metadata.final_png_path)));
        end

        function ensureCapacity(obj, requiredLines, imageWidth)
            currentLines = size(obj.RgbCubeRaw, 1);
            if currentLines >= requiredLines
                return;
            end

            targetLines = max(requiredLines, max(64, currentLines * 2));
            expanded = zeros(targetLines, imageWidth, 3, "uint16");
            if currentLines > 0
                expanded(1:currentLines, :, :) = obj.RgbCubeRaw;
            end
            obj.RgbCubeRaw = expanded;
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
            if obj.LineCursor <= 0
                return;
            end

            raw = obj.RgbCubeRaw(1:obj.LineCursor, :, :);
            displayImage = zeros(size(raw, 1), size(raw, 2), 3, "double");
            for channel = 1:3
                plane = double(raw(:, :, channel));
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
    end
end
