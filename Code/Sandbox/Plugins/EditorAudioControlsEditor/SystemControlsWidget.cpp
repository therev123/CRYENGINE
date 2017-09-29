// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "SystemControlsWidget.h"

#include "SystemControlsModel.h"
#include "AudioControlsEditorPlugin.h"
#include "MiddlewareDataWidget.h"
#include "MiddlewareDataModel.h"
#include "AudioAssets.h"
#include "AudioAssetsManager.h"
#include "SystemControlsEditorIcons.h"
#include "AudioTreeView.h"

#include <CryAudio/IAudioSystem.h>
#include <IAudioSystemEditor.h>
#include <IAudioSystemItem.h>
#include <IEditor.h>
#include <QtUtil.h>
#include <EditorStyleHelper.h>
#include <FilePathUtil.h>
#include <CrySystem/File/CryFile.h>
#include <CryString/CryPath.h>
#include <CryMath/Cry_Camera.h>
#include <CryIcon.h>
#include <Controls/QuestionDialog.h>
#include <ProxyModels/MountingProxyModel.h>
#include <QSearchBox.h>
#include <IUndoObject.h>

#include <QAction>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>


namespace ACE
{
//////////////////////////////////////////////////////////////////////////
CSystemControlsWidget::CSystemControlsWidget(CAudioAssetsManager* pAssetsManager)
	: m_pAssetsManager(pAssetsManager)
	, m_pAssetsModel(new CSystemControlsModel(m_pAssetsManager))
	, m_pFilterProxyModel(new CSystemControlsFilterProxyModel(this))
	, m_pSearchBox(new QSearchBox())
	, m_pFilterButton(new QToolButton())
	, m_pFilterWidget(new QWidget())
	, m_pTreeView(new CAudioTreeView())
{
	auto const libCount = m_pAssetsManager->GetLibraryCount();
	m_libraryModels.resize(libCount);

	for (int i = 0; i < libCount; ++i)
	{
		m_libraryModels[i] = new CAudioLibraryModel(m_pAssetsManager, m_pAssetsManager->GetLibrary(i));
	}

	m_pMountingProxyModel = new CMountingProxyModel(WrapMemberFunction(this, &CSystemControlsWidget::CreateLibraryModelFromIndex));
	m_pMountingProxyModel->SetHeaderDataCallbacks(1, &GetHeaderData);
	m_pMountingProxyModel->SetSourceModel(m_pAssetsModel);

	m_pFilterProxyModel->setSourceModel(m_pMountingProxyModel);
	m_pFilterProxyModel->setDynamicSortFilter(true);
	
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	QVBoxLayout* const pMainLayout = new QVBoxLayout(this);
	pMainLayout->setContentsMargins(0, 0, 0, 0);

	QSplitter* const pSplitter = new QSplitter();
	pSplitter->setOrientation(Qt::Vertical);
	pSplitter->setChildrenCollapsible(false);

	InitFilterWidgets(pMainLayout);
	pSplitter->addWidget(m_pFilterWidget);

	m_pTreeView->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
	m_pTreeView->setDragEnabled(true);
	m_pTreeView->setDragDropMode(QAbstractItemView::DragDrop);
	m_pTreeView->setDefaultDropAction(Qt::MoveAction);
	m_pTreeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_pTreeView->setContextMenuPolicy(Qt::CustomContextMenu);
	m_pTreeView->setModel(m_pFilterProxyModel);
	m_pTreeView->sortByColumn(0, Qt::AscendingOrder);
	m_pTreeView->installEventFilter(this);
	pSplitter->addWidget(m_pTreeView);
	
	pSplitter->setStretchFactor(0, 0);
	pSplitter->setStretchFactor(1, 1);
	pMainLayout->addWidget(pSplitter);

	QObject::connect(m_pTreeView, &CAudioTreeView::customContextMenuRequested, this, &CSystemControlsWidget::OnContextMenu);
	QObject::connect(m_pTreeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &CSystemControlsWidget::SelectedControlChanged);
	QObject::connect(m_pTreeView->selectionModel(), &QItemSelectionModel::selectionChanged, m_pTreeView, &CAudioTreeView::OnSelectionChanged);
	QObject::connect(m_pTreeView->selectionModel(), &QItemSelectionModel::currentChanged, this, &CSystemControlsWidget::StopControlExecution);
	
	QObject::connect(m_pMountingProxyModel, &CMountingProxyModel::rowsInserted, this, &CSystemControlsWidget::SelectNewAsset);
	
	m_pAssetsManager->signalItemAboutToBeAdded.Connect([&](CAudioAsset* const pItem)
	{
		ResetFilters();
	}, reinterpret_cast<uintptr_t>(this));

	m_pAssetsManager->signalLibraryAboutToBeAdded.Connect([&]()
	{
		ResetFilters();
	}, reinterpret_cast<uintptr_t>(this));

	m_pAssetsManager->signalLibraryAboutToBeRemoved.Connect([&](CAudioLibrary* const pLibrary)
	{
		int const libCount = m_pAssetsManager->GetLibraryCount();

		for (int i = 0; i < libCount; ++i)
		{
			if (m_pAssetsManager->GetLibrary(i) == pLibrary)
			{
				m_libraryModels[i]->deleteLater();
				m_libraryModels.erase(m_libraryModels.begin() + i);
				break;
			}
		}
	}, reinterpret_cast<uintptr_t>(this));
}

//////////////////////////////////////////////////////////////////////////
CSystemControlsWidget::~CSystemControlsWidget()
{
	m_pAssetsManager->signalLibraryAboutToBeRemoved.DisconnectById(reinterpret_cast<uintptr_t>(this));
	m_pAssetsManager->signalItemAboutToBeAdded.DisconnectById(reinterpret_cast<uintptr_t>(this));
	m_pAssetsManager->signalLibraryAboutToBeAdded.DisconnectById(reinterpret_cast<uintptr_t>(this));

	StopControlExecution();
	delete m_pAssetsModel;

	int const libCount = m_libraryModels.size();

	for (int i = 0; i < libCount; ++i)
	{
		m_libraryModels[i]->DisconnectFromSystem();
		delete m_libraryModels[i];
	}

	m_libraryModels.clear();
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::InitAddControlWidget(QHBoxLayout* const pLayout)
{
	QPushButton* const pAddButton = new QPushButton(tr("Add"));
	pAddButton->setToolTip(tr("Add new library, folder or control"));
	
	QMenu* const pAddButtonMenu = new QMenu();
	QObject::connect(pAddButtonMenu, &QMenu::aboutToShow, this, &CSystemControlsWidget::UpdateCreateButtons);

	pAddButtonMenu->addAction(GetItemTypeIcon(EItemType::Library), tr("Library"), [&]()
	{
		m_isCreatedFromMenu = true;
		m_pAssetsManager->CreateLibrary(Utils::GenerateUniqueLibraryName("new_library", *m_pAssetsManager));
	});

	pAddButtonMenu->addSeparator();

	m_pCreateParentFolderAction = new QAction(GetItemTypeIcon(EItemType::Folder), tr("Parent Folder"), pAddButtonMenu);
	QObject::connect(m_pCreateParentFolderAction, &QAction::triggered, [&]() { CreateParentFolder(); });
	m_pCreateParentFolderAction->setVisible(false);
	pAddButtonMenu->addAction(m_pCreateParentFolderAction);

	m_pCreateFolderAction = new QAction(GetItemTypeIcon(EItemType::Folder), tr("Folder"), pAddButtonMenu);
	QObject::connect(m_pCreateFolderAction, &QAction::triggered, [&]() { CreateFolder(GetSelectedAsset()); });
	m_pCreateFolderAction->setVisible(false);
	pAddButtonMenu->addAction(m_pCreateFolderAction);

	m_pCreateTriggerAction = new QAction(GetItemTypeIcon(EItemType::Trigger), tr("Trigger"), pAddButtonMenu);
	QObject::connect(m_pCreateTriggerAction, &QAction::triggered, [&]() { CreateControl("new_trigger", EItemType::Trigger, GetSelectedAsset()); });
	m_pCreateTriggerAction->setVisible(false);
	pAddButtonMenu->addAction(m_pCreateTriggerAction);

	m_pCreateParameterAction = new QAction(GetItemTypeIcon(EItemType::Parameter), tr("Parameter"), pAddButtonMenu);
	QObject::connect(m_pCreateParameterAction, &QAction::triggered, [&]() { CreateControl("new_parameter", EItemType::Parameter, GetSelectedAsset()); });
	m_pCreateParameterAction->setVisible(false);
	pAddButtonMenu->addAction(m_pCreateParameterAction);

	m_pCreateSwitchAction = new QAction(GetItemTypeIcon(EItemType::Switch), tr("Switch"), pAddButtonMenu);
	QObject::connect(m_pCreateSwitchAction, &QAction::triggered, [&]() { CreateControl("new_switch", EItemType::Switch, GetSelectedAsset()); });
	m_pCreateSwitchAction->setVisible(false);
	pAddButtonMenu->addAction(m_pCreateSwitchAction);

	m_pCreateStateAction = new QAction(GetItemTypeIcon(EItemType::State), tr("State"), pAddButtonMenu);
	QObject::connect(m_pCreateStateAction, &QAction::triggered, [&]() { CreateControl("new_state", EItemType::State, GetSelectedAsset()); });
	m_pCreateStateAction->setVisible(false);
	pAddButtonMenu->addAction(m_pCreateStateAction);

	m_pCreateEnvironmentAction = new QAction(GetItemTypeIcon(EItemType::Environment), tr("Environment"), pAddButtonMenu);
	QObject::connect(m_pCreateEnvironmentAction, &QAction::triggered, [&]() { CreateControl("new_environment", EItemType::Environment, GetSelectedAsset()); });
	m_pCreateEnvironmentAction->setVisible(false);
	pAddButtonMenu->addAction(m_pCreateEnvironmentAction);

	m_pCreatePreloadAction = new QAction(GetItemTypeIcon(EItemType::Preload), tr("Preload"), pAddButtonMenu);
	QObject::connect(m_pCreatePreloadAction, &QAction::triggered, [&]() { CreateControl("new_preload", EItemType::Preload, GetSelectedAsset()); });
	m_pCreatePreloadAction->setVisible(false);
	pAddButtonMenu->addAction(m_pCreatePreloadAction);

	pAddButton->setMenu(pAddButtonMenu);
	pLayout->addWidget(pAddButton);
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::InitFilterWidgets(QVBoxLayout* const pMainLayout)
{
	QHBoxLayout* const pFilterLayout = new QHBoxLayout();
	pFilterLayout->addWidget(m_pSearchBox);

	m_pFilterButton->setIcon(CryIcon("icons:General/Filter.ico"));
	m_pFilterButton->setToolTip(tr("Show Control Type Filters"));
	m_pFilterButton->setCheckable(true);
	m_pFilterButton->setMaximumSize(QSize(20, 20));
	pFilterLayout->addWidget(m_pFilterButton);

	InitAddControlWidget(pFilterLayout);
	InitTypeFilters();

	QObject::connect(m_pFilterButton, &QToolButton::toggled, [&](bool const isChecked)
	{
		m_pFilterWidget->setVisible(isChecked);

		if (isChecked)
		{
			m_pFilterButton->setToolTip(tr("Hide Control Type Filters"));
		}
		else
		{
			m_pFilterButton->setToolTip(tr("Show Control Type Filters"));
		}
	});


	QObject::connect(m_pSearchBox, &QSearchBox::textChanged, [&](QString const& filter)
	{
		if (m_filter != filter)
		{
			if (m_filter.isEmpty() && !filter.isEmpty())
			{
				BackupTreeViewStates();
				StartTextFiltering();
				m_pTreeView->expandAll();
			}
			else if (!m_filter.isEmpty() && filter.isEmpty())
			{
				m_pFilterProxyModel->setFilterFixedString(filter);
				m_pTreeView->collapseAll();
				RestoreTreeViewStates();
				StopTextFiltering();
			}
			else if (!m_filter.isEmpty() && !filter.isEmpty())
			{
				m_pFilterProxyModel->setFilterFixedString(filter);
				m_pTreeView->expandAll();
			}

			m_filter = filter;
		}

		m_pFilterProxyModel->setFilterFixedString(filter);
	});

	pMainLayout->addLayout(pFilterLayout);
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::InitTypeFilters()
{
	QVBoxLayout* const pTypeFiltersLayout = new QVBoxLayout();
	pTypeFiltersLayout->setContentsMargins(0, 5, 0, 5);

	QCheckBox* const pFilterTriggersCheckbox = new QCheckBox(tr("Triggers"));
	QObject::connect(pFilterTriggersCheckbox, &QCheckBox::toggled, [&](bool const isVisible)
	{
		ShowControlType(EItemType::Trigger, isVisible);
	});

	pFilterTriggersCheckbox->setChecked(true);
	pFilterTriggersCheckbox->setToolTip(tr("Show/hide Triggers"));
	pFilterTriggersCheckbox->setIcon(GetItemTypeIcon(EItemType::Trigger));
	pTypeFiltersLayout->addWidget(pFilterTriggersCheckbox);

	QCheckBox* const pFilterParametersCheckbox = new QCheckBox(tr("Parameters"));
	QObject::connect(pFilterParametersCheckbox, &QCheckBox::toggled, [&](bool const isVisible)
	{
		ShowControlType(EItemType::Parameter, isVisible);
	});

	pFilterParametersCheckbox->setChecked(true);
	pFilterParametersCheckbox->setToolTip(tr("Show/hide Parameters"));
	pFilterParametersCheckbox->setIcon(GetItemTypeIcon(EItemType::Parameter));
	pTypeFiltersLayout->addWidget(pFilterParametersCheckbox);

	QCheckBox* const pFilterSwitchesCheckbox = new QCheckBox(tr("Switches"));
	QObject::connect(pFilterSwitchesCheckbox, &QCheckBox::toggled, [&](bool const isVisible)
	{
		ShowControlType(EItemType::Switch, isVisible);
	});

	pFilterSwitchesCheckbox->setChecked(true);
	pFilterSwitchesCheckbox->setToolTip(tr("Show/hide Switches"));
	pFilterSwitchesCheckbox->setIcon(GetItemTypeIcon(EItemType::Switch));
	pTypeFiltersLayout->addWidget(pFilterSwitchesCheckbox);

	QCheckBox* const pFilterEnvironmentsCheckbox = new QCheckBox(tr("Environments"));
	QObject::connect(pFilterEnvironmentsCheckbox, &QCheckBox::toggled, [&](bool const isVisible)
	{
		ShowControlType(EItemType::Environment, isVisible);
	});

	pFilterEnvironmentsCheckbox->setChecked(true);
	pFilterEnvironmentsCheckbox->setToolTip(tr("Show/hide Environments"));
	pFilterEnvironmentsCheckbox->setIcon(GetItemTypeIcon(EItemType::Environment));
	pTypeFiltersLayout->addWidget(pFilterEnvironmentsCheckbox);

	QCheckBox* const pFilterPreloadsCheckbox = new QCheckBox(tr("Preloads"));
	QObject::connect(pFilterPreloadsCheckbox, &QCheckBox::toggled, [&](bool const isVisible)
	{
		ShowControlType(EItemType::Preload, isVisible);
	});

	pFilterPreloadsCheckbox->setChecked(true);
	pFilterPreloadsCheckbox->setToolTip(tr("Show/hide Preloads"));
	pFilterPreloadsCheckbox->setIcon(GetItemTypeIcon(EItemType::Preload));
	pTypeFiltersLayout->addWidget(pFilterPreloadsCheckbox);

	m_pFilterWidget = QtUtil::MakeScrollable(pTypeFiltersLayout);
	m_pFilterWidget->setHidden(true);
}

//////////////////////////////////////////////////////////////////////////
bool CSystemControlsWidget::eventFilter(QObject* pObject, QEvent* pEvent)
{
	if ((pEvent->type() == QEvent::KeyRelease) && !m_pTreeView->IsEditing())
	{
		QKeyEvent const* const pKeyEvent = static_cast<QKeyEvent*>(pEvent);

		if (pKeyEvent != nullptr)
		{
			if (pKeyEvent->key() == Qt::Key_Delete)
			{
				DeleteSelectedControl();
			}
			else if (pKeyEvent->key() == Qt::Key_Space)
			{
				ExecuteControl();
			}
		}
	}
	return QWidget::eventFilter(pObject, pEvent);
}

//////////////////////////////////////////////////////////////////////////
std::vector<CAudioControl*> CSystemControlsWidget::GetSelectedControls()
{
	QModelIndexList const& indexes = m_pTreeView->selectionModel()->selectedIndexes();
	std::vector<CAudioLibrary*> libraries;
	std::vector<CAudioFolder*> folders;
	std::vector<CAudioControl*> controls;
	AudioModelUtils::GetAssetsFromIndices(indexes, libraries, folders, controls);
	return controls;
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::Reload()
{
	ResetFilters();
	m_pFilterProxyModel->invalidate();
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::ShowControlType(EItemType const type, bool const isVisible)
{
	m_pFilterProxyModel->EnableControl(isVisible, type);

	if (type == EItemType::Switch)
	{
		m_pFilterProxyModel->EnableControl(isVisible, EItemType::State);
	}
}

//////////////////////////////////////////////////////////////////////////
CAudioControl* CSystemControlsWidget::CreateControl(string const& name, EItemType type, CAudioAsset* const pParent)
{
	m_isCreatedFromMenu = true;

	if (type != EItemType::State)
	{
		return m_pAssetsManager->CreateControl(Utils::GenerateUniqueControlName(name, type, *m_pAssetsManager), type, pParent);
	}
	else
	{
		return m_pAssetsManager->CreateControl(Utils::GenerateUniqueName(name, type, pParent), type, pParent);
	}
}

//////////////////////////////////////////////////////////////////////////
CAudioAsset* CSystemControlsWidget::CreateFolder(CAudioAsset* const pParent)
{
	m_isCreatedFromMenu = true;
	return m_pAssetsManager->CreateFolder(Utils::GenerateUniqueName("new_folder", EItemType::Folder, pParent), pParent);
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::CreateParentFolder()
{
	std::vector<CAudioLibrary*> libraries;
	std::vector<CAudioFolder*> folders;
	std::vector<CAudioControl*> controls;
	std::vector<CAudioAsset*> assetsToMove;

	auto const& selection = m_pTreeView->selectionModel()->selectedRows();
	AudioModelUtils::GetAssetsFromIndices(selection, libraries, folders, controls);

	for (auto const pFolder : folders)
	{
		assetsToMove.emplace_back(static_cast<CAudioAsset*>(pFolder));
	}

	for (auto const pControl : controls)
	{
		assetsToMove.emplace_back(static_cast<CAudioAsset*>(pControl));
	}

	auto const pParent = assetsToMove[0]->GetParent();
	auto const pNewFolder = CreateFolder(pParent);
	m_pAssetsManager->MoveItems(pNewFolder, assetsToMove);
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::OnContextMenu(QPoint const& pos)
{
	QMenu* const pContextMenu = new QMenu();
	QMenu* const pAddMenu = new QMenu(tr("Add"));
	auto const& selection = m_pTreeView->selectionModel()->selectedRows();

	pContextMenu->addMenu(pAddMenu);
	pContextMenu->addSeparator();

	pAddMenu->addAction(GetItemTypeIcon(EItemType::Library), tr("Library"), [&]()
	{
		m_isCreatedFromMenu = true;
		m_pAssetsManager->CreateLibrary(Utils::GenerateUniqueLibraryName("new_library", *m_pAssetsManager));
	});

	pAddMenu->addSeparator();

	if (!selection.isEmpty())
	{
		std::vector<CAudioLibrary*> libraries;
		std::vector<CAudioFolder*> folders;
		std::vector<CAudioControl*> controls;

		AudioModelUtils::GetAssetsFromIndices(selection, libraries, folders, controls);

		if (IsParentFolderAllowed())
		{
			pAddMenu->addAction(GetItemTypeIcon(EItemType::Folder), tr("Parent Folder"), [&]() { CreateParentFolder(); });
		}

		if (selection.size() == 1)
		{
			QModelIndex const& index = m_pTreeView->currentIndex();

			if (index.isValid())
			{
				CAudioAsset const* const pAsset = AudioModelUtils::GetAssetFromIndex(index);

				if (pAsset != nullptr)
				{
					EItemType const assetType = pAsset->GetType();

					if ((assetType == EItemType::Library) || (assetType == EItemType::Folder))
					{
						CAudioAsset* pParent = nullptr;

						if (assetType == EItemType::Folder)
						{
							pParent = static_cast<CAudioAsset*>(folders[0]);
						}
						else
						{
							pParent = static_cast<CAudioAsset*>(libraries[0]);
						}

						pAddMenu->addAction(GetItemTypeIcon(EItemType::Folder), tr("Folder"), [&]() { CreateFolder(pParent); });
						pAddMenu->addAction(GetItemTypeIcon(EItemType::Trigger), tr("Trigger"), [&]() { CreateControl("new_trigger", EItemType::Trigger, pParent); });
						pAddMenu->addAction(GetItemTypeIcon(EItemType::Parameter), tr("Parameter"), [&]() { CreateControl("new_parameter", EItemType::Parameter, pParent); });
						pAddMenu->addAction(GetItemTypeIcon(EItemType::Switch), tr("Switch"), [&]() { CreateControl("new_switch", EItemType::Switch, pParent); });
						pAddMenu->addAction(GetItemTypeIcon(EItemType::Environment), tr("Environment"), [&]() { CreateControl("new_environment", EItemType::Environment, pParent); });
						pAddMenu->addAction(GetItemTypeIcon(EItemType::Preload), tr("Preload"), [&]() { CreateControl("new_preload", EItemType::Preload, pParent); });

						if (pParent->GetType() == EItemType::Library)
						{
							//TODO: Take into account files in the "levels" folder
							pContextMenu->addAction(tr("Open Containing Folder"), [&]()
							{
								QtUtil::OpenInExplorer(PathUtil::Make(PathUtil::GetGameProjectAssetsPath(), Utils::GetAssetFolder() + pParent->GetName() + ".xml").c_str());
							});

							pContextMenu->addSeparator();
						}
					}
				}
			}

			if (controls.size() == 1)
			{
				CAudioControl* const pControl = controls[0];
				EItemType const controlType = pControl->GetType();

				if (controlType == EItemType::Trigger)
				{
					QAction* const pTriggerAction = new QAction(tr("Execute Trigger"), pContextMenu);
					QObject::connect(pTriggerAction, &QAction::triggered, [&]() { ExecuteControl(); });
					pContextMenu->insertSeparator(pContextMenu->actions().at(0));
					pContextMenu->insertAction(pContextMenu->actions().at(0), pTriggerAction);
				}
				else if (controlType == EItemType::Switch)
				{
					pAddMenu->addAction(GetItemTypeIcon(EItemType::State), tr("State"), [&]() { CreateControl("new_state", EItemType::State, pControl); });
				}
				else if (controlType == EItemType::Preload)
				{
					if (pControl->GetScope() == CCrc32::Compute("global") && !pControl->IsAutoLoad())
					{
						QAction* const pLoadAction = new QAction(tr("Load Global Preload Request"), pContextMenu);
						QAction* const pUnloadAction = new QAction(tr("Unload Global Preload Request"), pContextMenu);
						QObject::connect(pLoadAction, &QAction::triggered, [&]() { gEnv->pAudioSystem->PreloadSingleRequest(CryAudio::StringToId(pControl->GetName()), false); });
						QObject::connect(pUnloadAction, &QAction::triggered, [&]() { gEnv->pAudioSystem->UnloadSingleRequest(CryAudio::StringToId(pControl->GetName())); });
						pContextMenu->insertSeparator(pContextMenu->actions().at(0));
						pContextMenu->insertAction(pContextMenu->actions().at(0), pUnloadAction);
						pContextMenu->insertAction(pContextMenu->actions().at(0), pLoadAction);
					}
				}
			}
		}
		else if (controls.size() > 1)
		{
			bool hasOnlyGlobalPreloads = false;

			for (auto const pControl : controls)
			{
				if ((pControl->GetType() == EItemType::Preload) && (pControl->GetScope() == CCrc32::Compute("global")) && !pControl->IsAutoLoad())
				{
					hasOnlyGlobalPreloads = true;
				}
				else
				{
					hasOnlyGlobalPreloads = false;
					break;
				}
			}

			if (hasOnlyGlobalPreloads)
			{

				QAction* const pLoadAction = new QAction(tr("Load Global Preload Requests"), pContextMenu);
				QAction* const pUnloadAction = new QAction(tr("Unload Global Preload Requests"), pContextMenu);
				QObject::connect(pLoadAction, &QAction::triggered, [&]()
				{ 
					for (auto const pControl : controls)
					{
						gEnv->pAudioSystem->PreloadSingleRequest(CryAudio::StringToId(pControl->GetName()), false);
					}
				});

				QObject::connect(pUnloadAction, &QAction::triggered, [&]()
				{ 
					for (auto const pControl : controls)
					{
						gEnv->pAudioSystem->UnloadSingleRequest(CryAudio::StringToId(pControl->GetName()));
					}
				});

				pContextMenu->insertSeparator(pContextMenu->actions().at(0));
				pContextMenu->insertAction(pContextMenu->actions().at(0), pUnloadAction);
				pContextMenu->insertAction(pContextMenu->actions().at(0), pLoadAction);
			}
		}

		pContextMenu->addAction(tr("Rename"), [&]() { m_pTreeView->edit(m_pTreeView->currentIndex()); });
		pContextMenu->addAction(tr("Delete"), [&]() { DeleteSelectedControl(); });
		pContextMenu->addSeparator();
		pContextMenu->addAction(tr("Expand Selection"), [&]() { m_pTreeView->ExpandSelection(m_pTreeView->GetSelectedIndexes()); });
		pContextMenu->addAction(tr("Collapse Selection"), [&]() { m_pTreeView->CollapseSelection(m_pTreeView->GetSelectedIndexes()); });
		pContextMenu->addSeparator();
	}

	pContextMenu->addAction(tr("Expand All"), [&]() { m_pTreeView->expandAll(); });
	pContextMenu->addAction(tr("Collapse All"), [&]() { m_pTreeView->collapseAll(); });

	pContextMenu->exec(QCursor::pos());
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::DeleteSelectedControl()
{
	auto const& selection = m_pTreeView->selectionModel()->selectedRows();
	int const size = selection.length();

	if (size > 0)
	{
		QString text;

		if (size == 1)
		{
			text = R"(Are you sure you want to delete ")" + selection[0].data(Qt::DisplayRole).toString() + R"("?.)";
		}
		else
		{
			text = "Are you sure you want to delete the selected controls and folders?";
		}

		CQuestionDialog* const messageBox = new CQuestionDialog();
		messageBox->SetupQuestion("Audio Controls Editor", text);

		if (messageBox->Execute() == QDialogButtonBox::Yes)
		{
			CUndo undo("Audio Control Removed");

			std::vector<CAudioAsset*> selectedItems;

			for (auto const& index : selection)
			{
				if (index.isValid())
				{
					selectedItems.push_back(AudioModelUtils::GetAssetFromIndex(index));
				}
			}

			std::vector<CAudioAsset*> itemsToDelete;
			Utils::SelectTopLevelAncestors(selectedItems, itemsToDelete);

			for (auto const pItem : itemsToDelete)
			{
				m_pAssetsManager->DeleteItem(pItem);
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::ExecuteControl()
{
	CAudioAsset const* const pAsset = AudioModelUtils::GetAssetFromIndex(m_pTreeView->currentIndex());

	if ((pAsset != nullptr) && (pAsset->GetType() == EItemType::Trigger))
	{
		CAudioControlsEditorPlugin::ExecuteTrigger(pAsset->GetName());
	}
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::StopControlExecution()
{
	CAudioControlsEditorPlugin::StopTriggerExecution();
}

//////////////////////////////////////////////////////////////////////////
QAbstractItemModel* CSystemControlsWidget::CreateLibraryModelFromIndex(QModelIndex const& sourceIndex)
{
	if (sourceIndex.model() != m_pAssetsModel)
	{
		return nullptr;
	}

	size_t const numLibraries = m_libraryModels.size();
	size_t const row = static_cast<size_t>(sourceIndex.row());

	if (row >= numLibraries)
	{
		m_libraryModels.resize(row + 1);

		for (size_t i = numLibraries; i < row + 1; ++i)
		{
			m_libraryModels[i] = new CAudioLibraryModel(m_pAssetsManager, m_pAssetsManager->GetLibrary(i));
		}
	}

	return m_libraryModels[row];
}

//////////////////////////////////////////////////////////////////////////
CAudioAsset* CSystemControlsWidget::GetSelectedAsset() const
{
	QModelIndex const& index = m_pTreeView->currentIndex();

	if (index.isValid())
	{
		return AudioModelUtils::GetAssetFromIndex(index);
	}

	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::SelectNewAsset(QModelIndex const& parent, int const row)
{
	if (m_isCreatedFromMenu)
	{
		QModelIndex const& assetIndex = m_pFilterProxyModel->mapFromSource(m_pMountingProxyModel->index(row, 0, parent));
		m_pTreeView->setCurrentIndex(assetIndex);
		m_pTreeView->edit(assetIndex);
		m_isCreatedFromMenu = false;
	}
	else if (!CAudioControlsEditorPlugin::GetAssetsManager()->IsLoading())
	{
		QModelIndex const& parentIndex = m_pFilterProxyModel->mapFromSource(parent);
		m_pTreeView->expand(parentIndex);
		m_pTreeView->setCurrentIndex(parentIndex);
	}
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::UpdateCreateButtons()
{
	m_pCreateParentFolderAction->setVisible(IsParentFolderAllowed());

	auto const& selection = m_pTreeView->selectionModel()->selectedRows();

	if (selection.size() == 1)
	{
		QModelIndex const& index = m_pTreeView->currentIndex();

		if (index.isValid())
		{
			CAudioAsset const* const pAsset = AudioModelUtils::GetAssetFromIndex(index);

			if (pAsset != nullptr)
			{
				EItemType const itemType = pAsset->GetType();
				bool const isLibraryOrFolder = (itemType == EItemType::Library) || (itemType == EItemType::Folder);

				m_pCreateFolderAction->setVisible(isLibraryOrFolder);
				m_pCreateTriggerAction->setVisible(isLibraryOrFolder);
				m_pCreateParameterAction->setVisible(isLibraryOrFolder);
				m_pCreateSwitchAction->setVisible(isLibraryOrFolder);
				m_pCreateStateAction->setVisible(itemType == EItemType::Switch);
				m_pCreateEnvironmentAction->setVisible(isLibraryOrFolder);
				m_pCreatePreloadAction->setVisible(isLibraryOrFolder);
				return;
			}
		}
	}

	m_pCreateFolderAction->setVisible(false);
	m_pCreateTriggerAction->setVisible(false);
	m_pCreateParameterAction->setVisible(false);
	m_pCreateSwitchAction->setVisible(false);
	m_pCreateStateAction->setVisible(false);
	m_pCreateEnvironmentAction->setVisible(false);
	m_pCreatePreloadAction->setVisible(false);
}

//////////////////////////////////////////////////////////////////////////
bool CSystemControlsWidget::IsParentFolderAllowed()
{
	auto const& selection = m_pTreeView->selectionModel()->selectedRows();

	if (!selection.isEmpty())
	{
		std::vector<CAudioLibrary*> libraries;
		std::vector<CAudioFolder*> folders;
		std::vector<CAudioControl*> controls;

		AudioModelUtils::GetAssetsFromIndices(selection, libraries, folders, controls);

		if (libraries.empty() && (!folders.empty() || !controls.empty()))
		{
			bool isAllowed = true;
			CAudioAsset const* pParent;

			if (!folders.empty())
			{
				pParent = folders[0]->GetParent();

				for (auto const pFolder : folders)
				{
					if (pFolder->GetParent() != pParent)
					{
						isAllowed = false;
						break;
					}
				}
			}
			else
			{
				pParent = controls[0]->GetParent();
			}

			if (isAllowed)
			{
				for (auto const pControl : controls)
				{
					if ((pControl->GetParent() != pParent) || (pControl->GetType() == EItemType::State))
					{
						isAllowed = false;
						break;
					}
				}
			}

			return isAllowed;
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::ResetFilters()
{
	for (auto const pCheckBox : m_pFilterWidget->findChildren<QCheckBox*>())
	{
		pCheckBox->setChecked(true);
	}

	m_pSearchBox->clear();
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::BackupTreeViewStates()
{
	m_pTreeView->BackupExpanded();
	m_pTreeView->BackupSelection();
}

//////////////////////////////////////////////////////////////////////////
void CSystemControlsWidget::RestoreTreeViewStates()
{
	m_pTreeView->RestoreExpanded();
	m_pTreeView->RestoreSelection();
}

//////////////////////////////////////////////////////////////////////////
bool CSystemControlsWidget::IsEditing() const
{
	return m_pTreeView->IsEditing();
}
} // namespace ACE
