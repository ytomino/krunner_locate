#include "krunner_locate.hxx"
#include "query.hxx"
#include "use_locate.hxx"

#include <algorithm>
#include <cstdio>
#include <forward_list>
#include <map>
#include <set>

#include <sys/time.h>

#include <QDir>

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
	if(f != nullptr){
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

/* home */

static QByteArray home_path;
static QByteArray trash_path;
static QByteArray recent_documents_path;

static void setup_home_path()
{
	if(home_path.isEmpty()){
		home_path = QDir::homePath().toUtf8();
		home_path.append(u'/');
		trash_path = home_path;
		trash_path.append(QByteArrayLiteral(".local/share/Trash/"));
		recent_documents_path = home_path;
		recent_documents_path.append(
			QByteArrayLiteral(".local/share/RecentDocuments/")
		);
	}
}

static bool excluded(QByteArray const &path)
{
	return path.startsWith(trash_path) || path.startsWith(recent_documents_path);
}

/* QByteArray cache */
/* Note: QByteArray is reference counted. */

typedef std::set<QByteArray> QByteArray_set_t;

static QByteArray_set_t qbytearray_cache;

static QByteArray const &get_unique_qbytearray(QByteArray &&value)
{
	std::pair<QByteArray_set_t::iterator, bool> emplaced =
		qbytearray_cache.emplace(std::move(value));
	return *emplaced.first;
}

/* locate cache */

typedef std::map<locate_query_t, std::forward_list<QByteArray>> locate_cache_t;
static locate_cache_t locate_cache;

static std::forward_list<QByteArray> const *locate_with_cache(
	locate_query_t const *locate_query)
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
				QByteArray bytearray(item.data(), item.size());
				if(! excluded(bytearray)){
					iter->second.push_front(get_unique_qbytearray(std::move(bytearray)));
						/* descending order */
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

static bool hidden(QByteArray const &path)
{
	return path.contains(QByteArrayLiteral("/."));
}

static std::size_t count_units(QByteArray const &x, int position, int n);

static bool lt(QByteArray const &left, QByteArray const &right)
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
	
	int l_sep = left.lastIndexOf('/');
	int r_sep = right.lastIndexOf('/');
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

static std::time_t const interval = 60;

struct queried_t {
	std::forward_list<QByteArray> list;
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
		std::forward_list<QByteArray> const *list =
			locate_with_cache(&iter->first.locate_query);
		std::size_t n = 0;
		for(
			std::forward_list<QByteArray>::const_iterator i = list->cbegin();
			i != list->cend();
			++ i
		){
			if(filter_query(stringview_of_qbytearray(&*i), &iter->first)){
				iter->second.list.push_front(*i); /* ascending order */
				++ n;
			}
		}
		iter->second.list.sort(lt);
		iter->second.max_length = n;
		iter->second.last_checked_time = now;
	}else if(now - iter->second.last_checked_time > interval){
		/* remove the paths removed after those were cached */
		iter->second.list.remove_if(
			[&query](QByteArray const &item){
				return ! refilter_query(stringview_of_qbytearray(&item), &query);
			}
		);
		iter->second.last_checked_time = now;
	}
	return &iter->second;
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

/* icon cache */

struct icon_t {
	QString icon_name;
	std::time_t last_checked_time;
	
	icon_t() = default;
	icon_t(icon_t &&) = default;
};

typedef std::map<QByteArray, icon_t> icon_cache_t;
static icon_cache_t icon_cache;

static QString const &icon_with_cache(
	QByteArray const &path, QUrl const &url, std::time_t now
)
{
	if(hidden(path)){
		return hidden_icon;
	}else{
		std::pair<icon_cache_t::iterator, bool> emplaced =
			icon_cache.try_emplace(path);
		icon_cache_t::iterator iter = emplaced.first;
		if(emplaced.second){ /* || now - iter->second.last_checked_time > interval */
			iter->second.icon_name = get_unique_qstring(KIO::iconNameForUrl(url));
			iter->second.last_checked_time = now;
			
#ifdef LOGGING
			qDebug(
				"%s: icon_with_cache: %.*s, %s",
				log_name, path.size(), path.data(), qPrintable(iter->second.icon_name)
			);
#endif
		}
		return iter->second.icon_name;
	}
}

static void clear_old_icon_cache(std::time_t now)
{
#ifdef LOGGING
	icon_cache_t::size_type old_size = icon_cache.size();
#endif
	
	std::erase_if(
		icon_cache,
		[now](icon_cache_t::value_type const &item){
			return now - item.second.last_checked_time > interval;
		}
	);
	
#ifdef LOGGING
	if(icon_cache.size() != old_size){
		qDebug("%s: clear_old_icon_cache.", log_name);
	}
#endif
}

static void clear_cache()
{
#ifdef LOGGING
	qDebug("%s: clear_cache.", log_name);
#endif
	
	icon_cache.clear();
	qstring_cache.clear();
	query_cache.clear();
	locate_cache.clear();
	qbytearray_cache.clear();
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

static bool check_locate_mtime()
{
	std::time_t mtime;
	if(locate_mtime(&mtime) != 0){
		return false; /* error */
	}
	bool modified = mtime != last_locate_mtime;
	if(modified){ /* updatedb is executed */
		clear_cache();
		last_locate_mtime = mtime;
	}
	return modified;
}

static std::time_t last_use_time = -(interval + 1);

static bool update_time(std::time_t now)
{
	std::time_t old = last_use_time;
	last_use_time = now;
	return now - old > interval;
}

/* LocateRunner */

static QString const open_folder_icon = QStringLiteral("document-open-folder");

LocateRunner::LocateRunner(
	QObject *parent, KPluginMetaData const &pluginMetaData,
	QVariantList const &args
)
	: KRunner::AbstractRunner(parent, pluginMetaData, args),
		open_containing_folder_action(
			QIcon::fromTheme(open_folder_icon), QStringLiteral("Open Containing Folder")
		),
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
	this->setMinLetterCount(2);
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
		bool cleared;
		if(update_time(now)){
			cleared = check_locate_mtime();
		}else{
			cleared = false;
		}
		if(! cleared){
			clear_old_icon_cache(now);
		}
	}
	
	QByteArray query_utf8 = query_string.toUtf8();
	query_t query;
	parse_query(stringview_of_qbytearray(&query_utf8), &query);
	queried_t const *queried = query_with_cache(std::move(query), now);
	double n = 0.;
	for(
		std::forward_list<QByteArray>::const_iterator iter = queried->list.cbegin();
		iter != queried->list.cend();
		++ iter
	){
		int sep = iter->lastIndexOf('/');
		if(sep >= 0){
			QUrl url(
				QStringLiteral("file://")
					+ QString::fromLatin1(iter->toPercentEncoding(QByteArrayLiteral("/"))),
				QUrl::StrictMode
			);
			int base_name_length = iter->size() - (sep + 1);
			char const *base_name = iter->data() + (iter->size() - base_name_length);
			int dir_name_length;
			QByteArray dir_name;
			if(iter->startsWith(home_path)){
				dir_name_length = 2 + sep - home_path.size();
				int position = home_path.size() - 1;
				dir_name.reserve(dir_name_length);
				dir_name.append('~');
				dir_name.append(iter->data() + position, sep - position);
			}else{
				dir_name_length = sep;
				dir_name = *iter;
			}
			double relevance = 0.25 * (1. - n / queried->max_length); /* keep sorted */
			KRunner::QueryMatch match(this);
			match.setId(url.toString());
			match.setUrls(QList<QUrl>{url});
			match.setText(QString::fromUtf8(base_name, base_name_length));
			match.setSubtext(QString::fromUtf8(dir_name.constData(), dir_name_length));
			match.setIconName(icon_with_cache(*iter, url, now));
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

/* ICU */
#include <unicode/uiter.h>

static std::size_t count_units(QByteArray const &x, int position, int n)
{
	std::size_t result = 0;
	UCharIterator iter;
	uiter_setUTF8(&iter, x.data() + position, n);
	while(iter.hasNext(&iter) != 0){
		iter.next(&iter);
		++ result;
	}
	return result;
}

/* moc */

K_PLUGIN_CLASS_WITH_JSON(LocateRunner, "krunner_locate.json")

#include "moc_krunner_locate.cpp"
#include "krunner_locate.moc"
