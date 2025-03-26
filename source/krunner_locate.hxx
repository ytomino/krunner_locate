#ifndef KRUNNER_LOCATE_HXX
#define KRUNNER_LOCATE_HXX

#include <KRunner/AbstractRunner>

class LocateRunner : public KRunner::AbstractRunner {
	Q_OBJECT
	
public:
	LocateRunner(
		QObject *parent, KPluginMetaData const &pluginMetaData,
		QVariantList const &args
	);
	void reloadConfiguration() override;
	void match(KRunner::RunnerContext &context) override;
	void run(
		KRunner::RunnerContext const &context, KRunner::QueryMatch const &match
	) override;
};

#endif
