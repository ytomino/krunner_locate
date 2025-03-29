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

static char const log_name[] = "krunner_locate";

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

/* home */

static QString home_path;
static QString trash_path;

static void setup_home_path()
{
	if(home_path.isEmpty()){
		home_path = QDir::homePath();
		home_path.append(QChar('/'));
		trash_path = home_path;
		trash_path.append(QStringLiteral(".local/share/Trash/"));
	}
}

/* QString cache */
/* Note: QString is reference counted. */

typedef std::set<QString> QString_set_t;

static QString_set_t qstring_cache;

static QString get_unique_qstring(QString &&value)
{
	std::pair<QString_set_t::iterator, bool> emplaced =
		qstring_cache.emplace(std::move(value));
	return *emplaced.first;
}

/* locate cache */

typedef std::map<locate_query_t, std::unique_ptr<QStringList>> locate_cache_t;
static locate_cache_t locate_cache;

static QStringList const *locate_with_cache(locate_query_t const *locate_query)
{
	std::pair<locate_cache_t::iterator, bool> emplaced =
		locate_cache.try_emplace(*locate_query, nullptr);
	locate_cache_t::iterator iter = emplaced.first;
	if(emplaced.second){
		iter->second.reset(new QStringList);
		
		int status;
		int error = locate(
			locate_query->pattern,
			locate_query->base_name,
			locate_query->ignore_case,
			[iter](std::string_view item){
				iter->second->append(
					get_unique_qstring(QString::fromUtf8(item.data(), item.size()))
				);
				return 0;
			},
			&status
		);
		if(error != 0){
			iter->second->clear();
		}
	}
	return iter->second.get();
}

/* query cache */

static QString const regular_icon = QStringLiteral("document-open-symbolic");
static QString const dir_icon = QStringLiteral("folder-open-symbolic");
static QString const hidden_icon = QStringLiteral("view-hidden");

struct queried_item_t {
	QString path;
	QString const *icon;
};

static bool lt(queried_item_t const &left, queried_item_t const &right)
{
	bool l_not_in_home = ! left.path.startsWith(home_path);
	bool r_not_in_home = ! right.path.startsWith(home_path);
	if(l_not_in_home != r_not_in_home){
		return l_not_in_home < r_not_in_home;
	}
	
	bool l_hidden = left.icon == &hidden_icon;
	bool r_hidden = right.icon == &hidden_icon;
	if(l_hidden != r_hidden){
		return l_hidden < r_hidden;
	}
	
	qsizetype l_sep = rfind_sep(left.path);
	qsizetype r_sep = rfind_sep(right.path);
	if(l_sep < 0 || r_sep < 0){
		return false; /* something wrong */
	}
	
	std::size_t l_base_name_length = left.path.size() - (l_sep + 1);
	std::size_t r_base_name_length = right.path.size() - (r_sep + 1);
	if(l_base_name_length != r_base_name_length){
		return l_base_name_length < r_base_name_length;
	}
	
	if(l_sep != r_sep){
		return l_sep < r_sep;
	}
	
	return false;
		/* std::forward_list::sort preserves the order of equivalent elements */
}

static bool unexisting(queried_item_t const &x)
{
	return ! QFileInfo::exists(x.path);
}

typedef std::forward_list<queried_item_t> queried_list_t;

static std::time_t const interval = 60;

struct queried_t {
	queried_list_t list;
	std::size_t max_length;
	std::time_t last_checked_time;
};

typedef std::map<query_t, std::unique_ptr<queried_t>> query_cache_t;
static query_cache_t query_cache;

static queried_t const *query_with_cache(query_t const *query, std::time_t now)
{
	std::pair<query_cache_t::iterator, bool> emplaced =
		query_cache.try_emplace(*query, nullptr);
	query_cache_t::iterator iter = emplaced.first;
	if(emplaced.second){
		iter->second.reset(new queried_t);
		
		QStringList const *list = locate_with_cache(&query->locate_query);
		for(
			QStringList::const_iterator i = list->cbegin();
			i != list->cend();
			++ i
		){
			QByteArray utf8 = i->toUtf8();
			filtered_status_t filtered_status =
				filter_query(stringview_of_qbytearray(&utf8), query);
			if(filtered_status != fs_error){
				QString const *icon;
				switch(filtered_status){
				case fs_regular:
					icon = &regular_icon;
					break;
				case fs_directory:
					icon = &dir_icon;
					break;
				default:
					icon = nullptr;
				}
				if(icon != nullptr){
					if(i->contains(QStringLiteral("/."))){
						if(i->startsWith(trash_path)){
							icon = nullptr;
						}else{
							icon = &hidden_icon;
						}
					}
					if(icon != nullptr){
						iter->second->list.push_front(queried_item_t{*i, icon});
					}
				}
			}
		}
		iter->second->list.reverse();
		iter->second->list.sort(lt);
		iter->second->max_length = list->size();
		iter->second->last_checked_time = now;
	}else if(now - iter->second->last_checked_time > interval){
		/* remove the paths removed after those were cached */
		iter->second->list.remove_if(unexisting);
		iter->second->last_checked_time = now;
	}
	return iter->second.get();
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
static std::time_t last_use_time = -(interval + 1);

static void update_time(std::time_t now)
{
	std::time_t old = last_use_time;
	last_use_time = now;
	if(now - old > interval){
		std::time_t mtime;
		if(locate_mtime(&mtime) != 0){
			return; /* error */
		}
		if(mtime != last_locate_mtime){ /* updatedb is executed */
			clear_cache();
			last_locate_mtime = mtime;
		}
	}
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
		update_time(now);
	}
	
	QByteArray query_utf8 = query_string.toUtf8();
	query_t query;
	parse_query(stringview_of_qbytearray(&query_utf8), &query);
	queried_t const *queried = query_with_cache(&query, now);
	double n = 0.;
	for(
		queried_list_t::const_iterator iter = queried->list.cbegin();
		iter != queried->list.cend();
		++ iter
	){
		qsizetype sep = rfind_sep(iter->path);
		if(sep >= 0){
			QUrl url = QUrl::fromLocalFile(iter->path);
			QString dir_name = iter->path.left(sep);
			if(iter->path.startsWith(home_path)){
				dir_name.replace(0, home_path.size() - 1, QChar('~'));
			}
			double relevance = 0.25 * (1. - n / queried->max_length); /* keep sorted */
			KRunner::QueryMatch match(this);
			match.setId(url.toString());
			match.setUrls(QList<QUrl>{url});
			match.setText(iter->path.right(iter->path.size() - (sep + 1)));
			match.setSubtext(dir_name);
			match.setIconName(*iter->icon);
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
