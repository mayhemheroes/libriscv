#include "server.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

static const char* ADDRESS = "localhost";
static const uint16_t PORT = 1234;
static const uint16_t CACHE_PORT = 80;

int main(void)
{
    using namespace httplib;
    Server svr;

    svr.Post("/compile", compile);
	svr.Post("/execute", execute);
	svr.Post("/",
		[] (const Request& req, Response& res) {
			// take the POST body and send it to a cache
			httplib::Client cli("localhost", CACHE_PORT);
			// find compiler method
			std::string method = "linux";
			auto mit = req.params.find("method");
			if (mit != req.params.end()) method = mit->second;

			const httplib::Headers headers = {
				{ "X-Method", method }
			};
			// get the source code in the body compiled
			auto cres = cli.Post("/compile", headers,
				req.body, "text/plain");
			if (cres != nullptr)
			{
				if (cres->status == 200) {
					// execute the resulting binary
					auto eres = cli.Post("/execute", headers,
						cres->body, "application/x-riscv");
					if (eres != nullptr)
					{
						// return output from execution back to client
						res.status = eres->status;
						res.body = eres->body;
					} else {
						res.status = 500;
					}
					return;
				}
				res.status = cres->status;
				res.body   = cres->body;
			} else {
				res.status = 500;
			}
		});

	printf("Listening on %s:%u\n", ADDRESS, PORT);
    svr.listen(ADDRESS, PORT);
}

void common_response_fields(httplib::Response& res, int status)
{
	res.status = status;
	res.set_header("Access-Control-Allow-Origin", "*");
	res.set_header("Access-Control-Expose-Headers", "*");
}

void load_file(const std::string& filename, std::vector<uint8_t>& result)
{
    size_t size = 0;
    FILE* f = fopen(filename.c_str(), "rb");
    if (f == NULL) {
		result.clear();
		return;
	}

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    result.resize(size);
    if (size != fread(result.data(), 1, size, f))
    {
		result.clear();
    }
    fclose(f);
}
