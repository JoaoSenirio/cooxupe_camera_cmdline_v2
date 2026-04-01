function server = run_specsensor_rgb_stream_server(host, port)
if nargin < 1 || isempty(host)
    host = "127.0.0.1";
end
if nargin < 2 || isempty(port)
    port = 55001;
end

server = SpecSensorRgbStreamServer(host, port);
server.start();
disp("SpecSensor RGB stream server iniciado.");
disp("Mantenha o objeto retornado vivo no workspace para continuar recebendo dados.");
end
