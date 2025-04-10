#include "krunner_locate.hxx"
#include "query.hxx"
#include "use_locate.hxx"

#include <cstdio>
#include <forward_list>
#include <map>
#include <set>

#include <sys/time.h>

#include <QDir>
#include <QFileInfo>

#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenFileManagerWindowJob>
#include <KIO/OpenUrlJob>
#include <krunner_version.h>

static char const (log_name [[maybe_unused]])[] = "krunner_locate";

//#define LOGGING
#ifdef LOGGING
static bool log_handler_installed = false;
static QtMessageHandler original_log_handler = nullptr;

static void log_handler(
	QtMsgType type, QMessageLogContext const &context, QString const &str
)
{
	FILE *f = std::fopen("/tmp/krunner.log", "a");
	if(f != NULL){
		QString const message = qFormatLogMessage(type, context, str);
		std::fprintf(f, "%s\n", qPrintable(message));
		std::fclose(f);
	}
	
	if(original_log_handler){
		original_log_handler(type, context, str);
	}
}

static void install_log_handler()
{
	if(! log_handler_installed){
		log_handler_installed = true;
		original_log_handler = qInstallMessageHandler(log_handler);
	}
}
#endif

static std::string_view stringview_of_qbytearray(QByteArray const *x)
{
	return std::string_view(
		x->constData(), static_cast<std::string_view::size_type>(x->size())
	);
}

static qsizetype rfind_sep(QStringView path)
{
	qsizetype sep = -1;
	for(qsizetype i = path.size() - 1; i >= 0; --i){
		if(path[static_cast<uint>(i)] == QChar('/')){
			sep = i;
			break;
		}
	}
	return sep;
}

static std::size_t count_units(QStringView x, int position, int n)
{
	std::size_t result = 0;
	int i = position;
	int end = i + n;
	while(i < end){
		++ result;
		QChar c = x[i];
		if(c.isHighSurrogate()){ /* do not check low part because it is always valid */
			i += 2;
		}else{
			++ i;
		}
	}
	return result;
}

/* home */

static QString home_path;
static QString trash_path;
static QString recent_documents_path;

static void setup_home_path()
{
	if(home_path.isEmpty()){
		home_path = QDir::homePath();
		home_path.append(QChar('/'));
		trash_path = home_path;
		trash_path.append(QStringLiteral(".local/share/Trash/"));
		recent_documents_path = home_path;
		recent_documents_path.append(QStringLiteral(".local/share/RecentDocuments/"));
	}
}

static bool excluded(QStringView path)
{
	return path.startsWith(trash_path) || path.startsWith(recent_documents_path);
}

/* QString cache */
/* Note: QString is reference counted. */

typedef std::set<QString> QString_set_t;

static QString_set_t qstring_cache;

static QString const &get_unique_qstring(QString &&value)
{
	std::pair<QString_set_t::iterator, bool> emplaced =
		qstring_cache.emplace(std::move(value));
	return *emplaced.first;
}

/* locate cache */

typedef std::map<locate_query_t, QStringList> locate_cache_t;
static locate_cache_t locate_cache;

static QStringList const *locate_with_cache(locate_query_t const *locate_query)
{
	std::pair<locate_cache_t::iterator, bool> emplaced =
		locate_cache.try_emplace(*locate_query);
	locate_cache_t::iterator iter = emplaced.first;
	if(emplaced.second){
		int status;
		int error = locate(
			locate_query->pattern,
			locate_query->base_name,
			locate_query->ignore_case,
			[iter](std::string_view item){
				QString string = QString::fromUtf8(item.data(), item.size());
				if(! excluded(string)){
					iter->second.append(get_unique_qstring(std::move(string)));
				}
				return 0;
			},
			&status
		);
		if(error != 0){
			iter->second.clear();
		}
	}
	return &iter->second;
}

/* query cache */

static QString const hidden_icon = QStringLiteral("view-hidden");

static bool hidden(QStringView path)
{
	return path.contains(QStringLiteral("/."));
}

static bool lt(QString const &left, QString const &right)
{
	bool l_not_in_home = ! left.startsWith(home_path);
	bool r_not_in_home = ! right.startsWith(home_path);
	if(l_not_in_home != r_not_in_home){
		return l_not_in_home < r_not_in_home;
	}
	
	bool l_hidden = hidden(left);
	bool r_hidden = hidden(right);
	if(l_hidden != r_hidden){
		return l_hidden < r_hidden;
	}
	
	qsizetype l_sep = rfind_sep(left);
	qsizetype r_sep = rfind_sep(right);
	if(l_sep < 0 || r_sep < 0){
		return false; /* something wrong */
	}
	
	std::size_t l_base_name_count =
		count_units(left, l_sep + 1, left.size() - (l_sep + 1));
	std::size_t r_base_name_count =
		count_units(right, r_sep + 1, right.size() - (r_sep + 1));
	if(l_base_name_count != r_base_name_count){
		return l_base_name_count < r_base_name_count;
	}
	
	std::size_t l_dir_name_count = count_units(left, 0, l_sep);
	std::size_t r_dir_name_count = count_units(right, 0, r_sep);
	if(l_dir_name_count != r_dir_name_count){
		return l_dir_name_count < r_dir_name_count;
	}
	
	return false;
		/* std::forward_list::sort preserves the order of equivalent elements */
}

static bool unexisting(QString const &x)
{
	return ! QFileInfo::exists(x);
}

typedef std::forward_list<QString> queried_list_t;

static std::time_t const interval = 60;

struct queried_t {
	queried_list_t list;
	std::size_t max_length;
	std::time_t last_checked_time;
	
	queried_t() = default;
	queried_t(queried_t &&) = default;
};

typedef std::map<query_t, queried_t> query_cache_t;
static query_cache_t query_cache;

static queried_t const *query_with_cache(query_t &&query, std::time_t now)
{
	std::pair<query_cache_t::iterator, bool> emplaced =
		query_cache.try_emplace(std::move(query));
	query_cache_t::iterator iter = emplaced.first;
	if(emplaced.second){
		QStringList const *list = locate_with_cache(&iter->first.locate_query);
		for(
			QStringList::const_iterator i = list->cbegin();
			i != list->cend();
			++ i
		){
			QByteArray utf8 = i->toUtf8();
			filtered_status_t filtered_status =
				filter_query(stringview_of_qbytearray(&utf8), &iter->first);
			switch(filtered_status){
			case fs_regular: case fs_directory:
				iter->second.list.push_front(*i);
				break;
			default: /* fs_error, fs_other */
				;
			}
		}
		iter->second.list.reverse();
		iter->second.list.sort(lt);
		iter->second.max_length = list->size();
		iter->second.last_checked_time = now;
	}else if(now - iter->second.last_checked_time > interval){
		/* remove the paths removed after those were cached */
		iter->second.list.remove_if(unexisting);
		iter->second.last_checked_time = now;
	}
	return &iter->second;
}

static void clear_cache()
{
#ifdef LOGGING
	qDebug("%s: clear_cache.", log_name);
#endif
	
	query_cache.clear();
	locate_cache.clear();
	qstring_cache.clear();
}

/* modification time */
/* Note: time_t is signed long in Linux */

static_assert(static_cast<std::time_t>(0) > static_cast<std::time_t>(-1));

static int get_now(std::time_t *time)
{
	struct timeval tv;
	if(gettimeofday(&tv, nullptr) < 0){
		return nonzero_errno(errno);
	}
	*time = tv.tv_sec;
	return 0;
}

static std::time_t last_locate_mtime = -1;

static void check_locate_mtime()
{
	std::time_t mtime;
	if(locate_mtime(&mtime) != 0){
		return; /* error */
	}
	if(mtime != last_locate_mtime){ /* updatedb is executed */
		clear_cache();
		last_locate_mtime = mtime;
	}
}

static std::time_t last_use_time = -(interval + 1);

static bool update_time(std::time_t now)
{
	std::time_t old = last_use_time;
	last_use_time = now;
	return now - old > interval;
}

/* LocateRunner */

LocateRunner::LocateRunner(
	QObject *parent, KPluginMetaData const &pluginMetaData,
	QVariantList const &args
)
	: KRunner::AbstractRunner(parent, pluginMetaData, args),
		open_containing_folder_action(QStringLiteral("Open Containing Folder")),
		actions{&this->open_containing_folder_action}
{
#ifdef LOGGING
	install_log_handler();
	qDebug("%s: constructor.", log_name);
#endif
	
	/* miscellany initialization */
	setup_home_path();
}

void LocateRunner::reloadConfiguration()
{
#ifdef LOGGING
	qDebug("%s: reloadConfiguration.", log_name);
#endif
	
	this->setMatchRegex(QRegularExpression(QStringLiteral("[*./?]")));
}

void LocateRunner::match(KRunner::RunnerContext &context)
{
	QString const query_string = context.query();
#ifdef LOGGING
	qDebug("%s: match: %s", log_name, qPrintable(query_string));
#endif
	
	std::time_t now;
	if(get_now(&now) != 0){
		now = 0; /* error */
	}else{
		if(update_time(now)){
			check_locate_mtime();
		}
	}
	
	QByteArray query_utf8 = query_string.toUtf8();
	query_t query;
	parse_query(stringview_of_qbytearray(&query_utf8), &query);
	queried_t const *queried = query_with_cache(std::move(query), now);
	double n = 0.;
	for(
		queried_list_t::const_iterator iter = queried->list.cbegin();
		iter != queried->list.cend();
		++ iter
	){
		qsizetype sep = rfind_sep(*iter);
		if(sep >= 0){
			QUrl url = QUrl::fromLocalFile(*iter);
			QString dir_name = iter->left(sep);
			if(iter->startsWith(home_path)){
				dir_name.replace(0, home_path.size() - 1, QChar('~'));
			}
			double relevance = 0.25 * (1. - n / queried->max_length); /* keep sorted */
			KRunner::QueryMatch match(this);
			match.setId(url.toString());
			match.setUrls(QList<QUrl>{url});
			match.setText(iter->right(iter->size() - (sep + 1)));
			match.setSubtext(dir_name);
			match.setIconName(hidden(*iter) ? hidden_icon : KIO::iconNameForUrl(url));
			match.setRelevance(relevance);
			match.setActions(this->actions);
			context.addMatch(match);
			n += 1.;
		}
	}
}

void LocateRunner::run(
	const KRunner::RunnerContext & /* context */, const KRunner::QueryMatch &match
)
{
	QAction const *selected = match.selectedAction();
#ifdef LOGGING
	qDebug(
		"%s: run: %s, %s",
		log_name, qPrintable(match.text()),
		(selected != nullptr) ? qPrintable(selected->text()) : "null"
	);
#endif
	
	QList<QUrl> urls = match.urls();
	if(selected == &this->open_containing_folder_action){
		KIO::highlightInFileManager(urls);
	}else{
		for(QList<QUrl>::const_iterator i = urls.cbegin(); i != urls.cend(); ++ i){
			KIO::OpenUrlJob *job = new KIO::OpenUrlJob(*i);
			
			/* setting like kioclient */
			job->setUiDelegate(
				KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, nullptr)
			);
			job->setRunExecutables(false);
			job->setFollowRedirections(false);
			
			job->start();
		}
	}
}

K_PLUGIN_CLASS_WITH_JSON(LocateRunner, "krunner_locate.json")

#include "moc_krunner_locate.cpp"
#include "krunner_locate.moc"
