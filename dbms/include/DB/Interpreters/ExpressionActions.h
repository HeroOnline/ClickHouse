#pragma once

#include <DB/DataTypes/IDataType.h>
#include <DB/Interpreters/Settings.h>
#include <DB/Core/Names.h>
#include <DB/Core/ColumnWithNameAndType.h>
#include <DB/Core/Block.h>
#include <set>


namespace DB
{

class IFunction;
typedef Poco::SharedPtr<IFunction> FunctionPtr;
	
typedef std::pair<std::string, std::string> NameWithAlias;
typedef std::vector<NameWithAlias> NamesWithAliases;
typedef std::set<String> NameSet;
typedef std::map<String, String> NameToNameMap;

/** Содержит последовательность действий над блоком.
  */
class ExpressionActions
{
public:
	struct Action
	{
	private:
		Action() {}
		
	public:
		enum Type
		{
			APPLY_FUNCTION,
			ADD_COLUMN,
			REMOVE_COLUMN,
			COPY_COLUMN,
			ARRAY_JOIN, /// Заменяет указанные столбцы с массивами на столбцы с элементами. Размножает значения в остальных столбцах по количеству элементов в массивах. Массивы должны быть параллельными (иметь одинаковые длины).
			PROJECT, /// Переупорядочить и переименовать столбцы, удалить лишние. Допускаются одинаковые имена столбцов в результате.
		};
		
		Type type;
		
		std::string source_name;
		std::string result_name;
		DataTypePtr result_type;
		
		/// Для ARRAY_JOIN
		NameSet array_joined_columns;
		
		/// Для ADD_COLUMN.
		ColumnPtr added_column;
		
		/// Для APPLY_FUNCTION.
		mutable FunctionPtr function; /// mutable - чтобы можно было делать execute.
		Names argument_names;
		Names prerequisite_names;
		
		/// Для PROJECT.
		NamesWithAliases projection;
		
		/// Если result_name_ == "", в качестве имени используется "имя_функци(аргументы через запятую)".
		static Action applyFunction(FunctionPtr function_, const std::vector<std::string> & argument_names_, std::string result_name_ = "");
		
		static Action addColumn(ColumnWithNameAndType added_column_)
		{
			Action a;
			a.type = ADD_COLUMN;
			a.result_name = added_column_.name;
			a.result_type = added_column_.type;
			a.added_column = added_column_.column;
			return a;
		}
		
		static Action removeColumn(const std::string & removed_name)
		{
			Action a;
			a.type = REMOVE_COLUMN;
			a.source_name = removed_name;
			return a;
		}
		
		static Action copyColumn(const std::string & from_name, const std::string & to_name)
		{
			Action a;
			a.type = COPY_COLUMN;
			a.source_name = from_name;
			a.result_name = to_name;
			return a;
		}
		
		static Action project(const NamesWithAliases & projected_columns_)
		{
			Action a;
			a.type = PROJECT;
			a.projection = projected_columns_;
			return a;
		}
		
		static Action project(const Names & projected_columns_)
		{
			Action a;
			a.type = PROJECT;
			a.projection.resize(projected_columns_.size());
			for (size_t i = 0; i < projected_columns_.size(); ++i)
				a.projection[i] = NameWithAlias(projected_columns_[i], "");
			return a;
		}
		
		static Action arrayJoin(const NameSet & array_joined_columns)
		{
			if (array_joined_columns.empty())
				throw Exception("No arrays to join", ErrorCodes::LOGICAL_ERROR);
			Action a;
			a.type = ARRAY_JOIN;
			a.array_joined_columns = array_joined_columns;
			return a;
		}
		
		/// Какие столбцы нужны, чтобы выполнить это действие.
		/// Если этот Action еще не добавлен в ExpressionActions, возвращаемый список может быть неполным, потому что не учтены prerequisites.
		Names getNeededColumns() const;
		
		std::string toString() const;
		
	private:
		friend class ExpressionActions;
		
		std::vector<Action> getPrerequisites(Block & sample_block);
		void prepare(Block & sample_block);
		void execute(Block & block) const;
	};
	
	typedef std::vector<Action> Actions;
	
	ExpressionActions(const NamesAndTypesList & input_columns_, const Settings & settings_)
		: input_columns(input_columns_), settings(settings_)
	{
		for (NamesAndTypesList::iterator it = input_columns.begin(); it != input_columns.end(); ++it)
		{
			sample_block.insert(ColumnWithNameAndType(NULL, it->second, it->first));
		}
	}
	
	/// Для константных столбцов в input_columns_ могут содержаться сами столбцы.
	ExpressionActions(const ColumnsWithNameAndType & input_columns_, const Settings & settings_)
	: settings(settings_)
	{
		for (ColumnsWithNameAndType::const_iterator it = input_columns_.begin(); it != input_columns_.end(); ++it)
		{
			input_columns.push_back(NameAndTypePair(it->name, it->type));
			sample_block.insert(*it);
		}
	}
	
	/// Добавить входной столбец.
	/// Название столбца не должно совпадать с названиями промежуточных столбцов, возникающих при вычислении выражения.
	/// В выражении не должно быть действий PROJECT.
	void addInput(const ColumnWithNameAndType & column);
	void addInput(const NameAndTypePair & column);
	
	void add(const Action & action);
	
	/// Кладет в out_new_columns названия новых столбцов
	///  (образовавшихся в результате добавляемого действия и его rerequisites).
	void add(const Action & action, Names & out_new_columns);
	
	/// Добавляет в начало удаление всех лишних столбцов.
	void prependProjectInput();
	
	/// - Добавляет действия для удаления всех столбцов, кроме указанных.
	/// - Убирает неиспользуемые входные столбцы.
	/// - Может как-нибудь оптимизировать выражение.
	/// - Не переупорядочивает столбцы.
	/// - Не удаляет "неожиданные" столбцы (например, добавленные функциями).
	/// - Если output_columns пуст, оставляет один произвольный столбец (чтобы не потерялось количество строк в блоке).
	void finalize(const Names & output_columns);
	
	/// Получить список входных столбцов.
	Names getRequiredColumns() const
	{
		Names names;
		for (NamesAndTypesList::const_iterator it = input_columns.begin(); it != input_columns.end(); ++it)
			names.push_back(it->first);
		return names;
	}
	
	const NamesAndTypesList & getRequiredColumnsWithTypes() const { return input_columns; }

	/// Выполнить выражение над блоком. Блок должен содержать все столбцы , возвращаемые getRequiredColumns.
	void execute(Block & block) const;

	/// Получить блок-образец, содержащий имена и типы столбцов результата.
	const Block & getSampleBlock() const { return sample_block; }
	
	std::string getID() const;
	
	std::string dumpActions() const;
	
	static std::string getSmallestColumn(const NamesAndTypesList & columns);

private:
	NamesAndTypesList input_columns;
	Actions actions;
	Block sample_block;
	Settings settings;
	
	void checkLimits(Block & block) const;
	
	/// Добавляет сначала все prerequisites, потом само действие.
	/// current_names - столбцы, prerequisites которых сейчас обрабатываются.
	void addImpl(Action action, NameSet & current_names, Names & new_names);
	
	/// Попробовать что-нибудь улучшить, не меняя списки входных и выходных столбцов.
	void optimize();
	/// Переместить все arrayJoin как можно ближе к концу.
	void optimizeArrayJoin();
};

typedef SharedPtr<ExpressionActions> ExpressionActionsPtr;


/** Последовательность преобразований над блоком.
  * Предполагается, что результат каждого шага подается на вход следующего шага.
  * Используется для выполнения некоторых частей запроса по отдельности.
  * 
  * Например, можно составить цепочку из двух шагов:
  * 	1) вычислить выражение в секции WHERE,
  * 	2) вычислить выражение в секции SELECT,
  *  и между двумя шагами делать фильтрацию по значению в секции WHERE.
  */
struct ExpressionActionsChain
{
	struct Step
	{
		ExpressionActionsPtr actions;
		Names required_output;
		
		Step(ExpressionActionsPtr actions_ = NULL, Names required_output_ = Names())
			: actions(actions_), required_output(required_output_) {}
	};
	
	typedef std::vector<Step> Steps;
	
	Settings settings;
	Steps steps;
	
	void addStep()
	{
		if (steps.empty())
			throw Exception("Cannot add action to empty ExpressionActionsChain", ErrorCodes::LOGICAL_ERROR);
		
		ColumnsWithNameAndType columns = steps.back().actions->getSampleBlock().getColumns();
		steps.push_back(Step(new ExpressionActions(columns, settings)));
	}
	
	void finalize()
	{
		for (int i = static_cast<int>(steps.size()) - 1; i >= 0; --i)
		{
			steps[i].actions->finalize(steps[i].required_output);
			
			if (i > 0)
			{
				Names & previous_output = steps[i - 1].required_output;
				const NamesAndTypesList & columns = steps[i].actions->getRequiredColumnsWithTypes();
				for (NamesAndTypesList::const_iterator it = columns.begin(); it != columns.end(); ++it)
					previous_output.push_back(it->first);
				
				std::sort(previous_output.begin(), previous_output.end());
				previous_output.erase(std::unique(previous_output.begin(), previous_output.end()), previous_output.end());
				
				/// Если на выходе предыдущего шага образуются ненужные столбцы, добавим в начало этого шага их выбрасывание.
				/// За исключением случая, когда мы выбросим все столбцы и потеряем количество строк в блоке.
				if (!steps[i].actions->getRequiredColumnsWithTypes().empty()
					&& previous_output.size() > steps[i].actions->getRequiredColumnsWithTypes().size())
					steps[i].actions->prependProjectInput();
			}
		}
	}
	
	void clear()
	{
		steps.clear();
	}
	
	ExpressionActionsPtr getLastActions()
	{
		if (steps.empty())
			throw Exception("Empty ExpressionActionsChain", ErrorCodes::LOGICAL_ERROR);
		
		return steps.back().actions;
	}
	
	Step & getLastStep()
	{
		if (steps.empty())
			throw Exception("Empty ExpressionActionsChain", ErrorCodes::LOGICAL_ERROR);
		
		return steps.back();
	}
};

}
