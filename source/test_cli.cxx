#include "query.hxx"
#include "use_locate.hxx"

#include <cstdio>

int main(int argc, char const * const *argv)
{
	bool mtime = false;
	bool verbose = false;
	int i = 1;
	while(i < argc){
		std::string_view e(argv[i]);
		if(e == "--mtime"){
			++ i;
			mtime = true;
		}else if(e == "--verbose"){
			++ i;
			verbose = true;
		}else if(e== "--"){
			++ i;
			break;
		}else if(e.size() > 0 && e[0] == '-'){
			std::fprintf(stderr, "%s: unknown option: %s\n", argv[0], argv[i]);
			return 2;
		}else{
			break;
		}
	}
	int required_argc = mtime ? i : i + 1;
	if(argc < required_argc){
		std::fprintf(stderr, "%s: too few arguments.\n", argv[0]);
		return 2;
	}else if(argc > required_argc){
		std::fprintf(stderr, "%s: too many arguments.\n", argv[0]);
		return 2;
	}
	
	int error;
	if(mtime){
		std::time_t time;
		if((error = locate_mtime(&time)) != 0){
			std::fprintf(stderr, "%s: could not find any database.\n", argv[0]);
		}else{
			struct tm tmbuf;
			struct tm *tmr = localtime_r(&time, &tmbuf);
			if(tmr == nullptr){
				error = nonzero_errno(errno);
				std::fprintf(stderr, "%s: localtime_r failed.\n", argv[0]);
			}else{
				char buf[1024];
				std::strftime(buf, sizeof(buf) - 1, "%F %T %z", tmr);
				std::printf("%s\n", buf);
			}
		}
	}else{
		query_t query;
		parse_query(std::string_view(argv[i]), &query);
		
		if(verbose){
			std::fprintf(
				stderr, "%s: pattern=%.*s\n", argv[0],
				static_cast<int>(query.locate_query.pattern.size()),
				query.locate_query.pattern.data()
			);
			std::fprintf(
				stderr, "%s: base_name=%d\n",  argv[0], query.locate_query.base_name
			);
			std::fprintf(
				stderr, "%s: ignore_case=%d\n",  argv[0], query.locate_query.ignore_case
			);
			std::fprintf(stderr, "%s: absolute=%d\n",  argv[0], query.absolute);
			std::string_view file_type_filter = image(query.file_type_filter);
			std::fprintf(
				stderr, "%s: file_type_filter=%.*s\n", argv[0],
				static_cast<int>(file_type_filter.size()), file_type_filter.data()
			);
		}
		
		int status;
		error = locate(
			query.locate_query.pattern,
			query.locate_query.base_name,
			query.locate_query.ignore_case,
			[&query](std::string_view item){
				if(filter_query(item, &query)){
					std::printf("%.*s\n", static_cast<int>(item.size()), item.data());
				}
				return 0;
			},
			&status
		);
	}
	return error ? EXIT_FAILURE : EXIT_SUCCESS;
}
