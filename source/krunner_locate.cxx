#include "krunner_locate.hxx"
#include "query.hxx"
#include "use_locate.hxx"

#include <cstdio>
#include <list>
#include <map>
#include <set>

#include <QDir>

#include <KProcess>
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

static void setup_home_path()
{
	if(home_path.isEmpty()){
		home_path = QDir::homePath();
		home_path.append(QChar('/'));
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
	
	return false; /* std::list::sort preserves the order of equivalent elements */
}

typedef std::list<queried_item_t> queried_list_t;
typedef std::map<query_t, std::unique_ptr<queried_list_t>> query_cache_t;
static query_cache_t query_cache;

static queried_list_t const *query_with_cache(query_t const *query)
{
	std::pair<query_cache_t::iterator, bool> emplaced =
		query_cache.try_emplace(*query, nullptr);
	query_cache_t::iterator iter = emplaced.first;
	if(emplaced.second){
		iter->second.reset(new queried_list_t);
		
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
						icon = &hidden_icon;
					}
					iter->second->push_back(queried_item_t{*i, icon});
				}
			}
		}
		iter->second->sort(lt);
	}
	return iter->second.get();
}

/* LocateRunner */

LocateRunner::LocateRunner(
	QObject *parent, KPluginMetaData const &pluginMetaData,
	QVariantList const &args
)
	: KRunner::AbstractRunner(parent, pluginMetaData, args)
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
	
	QByteArray query_utf8 = query_string.toUtf8();
	query_t query;
	parse_query(stringview_of_qbytearray(&query_utf8), &query);
	queried_list_t const *list = query_with_cache(&query);
	double n = 0.;
	for(
		queried_list_t::const_iterator iter = list->cbegin();
		iter != list->cend();
		++ iter
	){
		qsizetype sep = rfind_sep(iter->path);
		if(sep >= 0){
			QString dir_name = iter->path.left(sep);
			if(iter->path.startsWith(home_path)){
				dir_name.replace(0, home_path.size() - 1, QChar('~'));
			}
			double relevance = 0.25 * (1. - n / list->size()); /* keep sorted */
			KRunner::QueryMatch match(this);
			match.setId(QUrl::fromLocalFile(iter->path).toString());
			match.setText(iter->path.right(iter->path.size() - (sep + 1)));
			match.setSubtext(dir_name);
			match.setIconName(*iter->icon);
			match.setRelevance(relevance);
			context.addMatch(match);
			n += 1.;
		}
	}
}

static QString const open_command = QStringLiteral("kde-open");

void LocateRunner::run(
	const KRunner::RunnerContext & /* context */, const KRunner::QueryMatch &match
)
{
#ifdef LOGGING
	qDebug("%s: run: %s", log_name, qPrintable(match.text()));
#endif
	
	KProcess::startDetached(open_command, QStringList{match.id()});
}

K_PLUGIN_CLASS_WITH_JSON(LocateRunner, "krunner_locate.json")

#include "moc_krunner_locate.cpp"
#include "krunner_locate.moc"
